#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
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

    crd::log::flush();
    crd::log::shutdown();
    return (built && executed) ? 0 : 1;
}
