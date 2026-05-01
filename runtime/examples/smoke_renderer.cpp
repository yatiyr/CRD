#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/shader/shader.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_renderer, "SmokeRenderer", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

namespace
{
class FakeBuffer final : public crd::rhi::Buffer
{
public:
    explicit FakeBuffer(crd::rhi::BufferDesc desc) : m_desc(desc) {}
    [[nodiscard]] const crd::rhi::BufferDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] void* map() noexcept override { return nullptr; }
    void unmap() noexcept override {}

private:
    crd::rhi::BufferDesc m_desc{};
};

class FakePipeline final : public crd::rhi::Pipeline
{
public:
    explicit FakePipeline(crd::rhi::GraphicsPipelineDesc desc) : m_desc(std::move(desc)) {}
    [[nodiscard]] const crd::rhi::GraphicsPipelineDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::GraphicsPipelineDesc m_desc{};
};

class FakeImage final : public crd::rhi::Image
{
public:
    explicit FakeImage(crd::rhi::ImageDesc desc) : m_desc(desc) {}
    [[nodiscard]] const crd::rhi::ImageDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::ImageDesc m_desc{};
};

class FakeCommandBuffer final : public crd::rhi::CommandBuffer
{
public:
    void begin() override { CRD_LOG_INFO(g_log_smoke_renderer, "begin"); }
    void end() override { CRD_LOG_INFO(g_log_smoke_renderer, "end"); }
    void reset() override {}
    void begin_rendering(const crd::rhi::RenderingInfo& /*info*/) override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "begin_rendering");
    }
    void end_rendering() override { CRD_LOG_INFO(g_log_smoke_renderer, "end_rendering"); }
    void bind_pipeline(crd::rhi::Pipeline& /*pipeline*/) override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "bind_pipeline");
    }
    void bind_vertex_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/) override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "bind_vertex_buffer");
    }
    void draw(crd::u32 vertex_count, crd::u32 /*first_vertex*/) override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "draw vertices={}", vertex_count);
    }
    void transition_image(crd::rhi::Image& /*image*/, crd::rhi::ImageAccess from,
                          crd::rhi::ImageAccess to) noexcept override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "transition_image from={} to={}", static_cast<int>(from),
                     static_cast<int>(to));
        ++transition_count;
    }

    int transition_count = 0;
};

class SimpleResolver final : public crd::renderer::PipelineResolver
{
public:
    explicit SimpleResolver(crd::rhi::Pipeline& pipeline) : m_pipeline(pipeline) {}
    [[nodiscard]] crd::rhi::Pipeline*
    resolve_pipeline(const crd::shader::VariantPipelineDesc& /*handoff*/) noexcept override
    {
        return &m_pipeline;
    }

private:
    crd::rhi::Pipeline& m_pipeline;
};
} // namespace

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("renderer_smoke");
    desc.frontend_modules.push_back(
        {crd::containers::String(
             (fs::Path(CRD_SOURCE_DIR) / "runtime/examples/shaders/reflect_triangle.vert").generic().data()),
         crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back(
        {crd::containers::String(
             (fs::Path(CRD_SOURCE_DIR) / "runtime/examples/shaders/reflect_triangle.frag").generic().data()),
         crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);
    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto variant = runtime->request_variant(request, diagnostics);

    if (!variant.is_valid())
    {
        CRD_LOG_ERROR(g_log_smoke_renderer, "Variant compile failed: {}", diagnostics.message.c_str());
        return 1;
    }

    FakeBuffer vertex_buffer(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    FakePipeline pipeline({});

    crd::renderer::Renderable renderable;
    renderable.vertex_buffer = &vertex_buffer;
    renderable.vertex_count = 3;
    renderable.material_instance_id = 42;
    renderable.variant = variant;

    crd::renderer::Renderer renderer;
    renderer.submit(renderable);

    crd::renderer::FramePlan plan;
    const bool built = renderer.build_frame({}, *runtime, plan);
    CRD_LOG_INFO(g_log_smoke_renderer, "build_frame={} draw_items={}", built, plan.draw_items.size());
    if (built && !plan.draw_items.empty())
    {
        const auto& item = plan.draw_items[0];
        CRD_LOG_INFO(g_log_smoke_renderer, "modules={} descriptors={} push_constants={} vertex_attributes={}",
                     item.handoff.modules.size(), item.handoff.descriptor_bindings.size(),
                     item.handoff.push_constants.size(), item.handoff.vertex_attributes.size());
    }

    FakeCommandBuffer command_buffer;
    SimpleResolver resolver(pipeline);
    const bool executed = renderer.execute_frame(
        plan,
        {{1280, 720},
         {nullptr, crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.08f, 0.08f, 0.12f, 1.0f}},
         nullptr},
        command_buffer, resolver);
    CRD_LOG_INFO(g_log_smoke_renderer, "execute_frame={}", executed);

    // --- Frame graph smoke ---
    // Demonstrates: import, multi-pass declaration, build, execute with barrier insertion.
    FakeCommandBuffer fg_cmd;
    crd::renderer::FrameGraph frame_graph;

    // Simulate a depth-prepass → main-color pass pipeline with fake external images.
    FakeImage depth_img({});
    FakeImage color_img({});
    auto depth_handle = frame_graph.import(&depth_img, crd::rhi::ImageAccess::Undefined);
    auto color_handle = frame_graph.import(&color_img, crd::rhi::ImageAccess::Undefined);

    {
        auto builder = frame_graph.add_pass("depth-prepass");
        builder.write(depth_handle, crd::rhi::ImageAccess::DepthWrite);
        builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    }
    {
        auto builder = frame_graph.add_pass("main-color");
        builder.read(depth_handle, crd::rhi::ImageAccess::DepthRead);
        builder.write(color_handle, crd::rhi::ImageAccess::ColorWrite);
        builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    }

    const bool fg_built = frame_graph.build();
    CRD_LOG_INFO(g_log_smoke_renderer, "frame_graph.build()={}", fg_built);

    // execute() with no transients; device is unused since all images are external (or null).
    struct FakeDevice final : crd::rhi::Device
    {
        [[nodiscard]] crd::rhi::BackendApi api() const noexcept override { return crd::rhi::BackendApi::Vulkan; }
        [[nodiscard]] std::unique_ptr<crd::rhi::Swapchain> create_swapchain(const crd::rhi::SwapchainDesc&) override
        {
            return nullptr;
        }
        [[nodiscard]] std::unique_ptr<crd::rhi::Buffer> create_buffer(const crd::rhi::BufferDesc&) override
        {
            return nullptr;
        }
        [[nodiscard]] std::unique_ptr<crd::rhi::Image> create_image(const crd::rhi::ImageDesc&) override
        {
            return nullptr;
        }
        [[nodiscard]] std::unique_ptr<crd::rhi::ShaderModule>
        create_shader_module(const crd::rhi::ShaderModuleDesc&) override
        {
            return nullptr;
        }
        [[nodiscard]] std::unique_ptr<crd::rhi::Pipeline>
        create_graphics_pipeline(const crd::rhi::GraphicsPipelineDesc&) override
        {
            return nullptr;
        }
        [[nodiscard]] std::unique_ptr<crd::rhi::CommandBuffer> create_command_buffer() override { return nullptr; }
        [[nodiscard]] crd::rhi::Queue& graphics_queue() noexcept override { return m_queue; }
        void wait_idle() override {}

        struct FakeQueue final : crd::rhi::Queue
        {
            bool submit(crd::rhi::CommandBuffer&, crd::rhi::Swapchain&) override { return true; }
            void present(crd::rhi::Swapchain&) override {}
            void wait_idle() override {}
        } m_queue;
    } fake_device;

    frame_graph.execute(fake_device, fg_cmd);
    // Expect 3 barriers: Undef→DepthWrite, Undef→ColorWrite, DepthWrite→DepthRead
    // (depth import starts Undefined; color import starts Undefined)
    CRD_LOG_INFO(g_log_smoke_renderer, "frame_graph.execute() transitions={}", fg_cmd.transition_count);
    const bool fg_ok = fg_built && fg_cmd.transition_count == 3;

    crd::log::flush();
    crd::log::shutdown();
    return (built && executed && fg_ok) ? 0 : 1;
}
