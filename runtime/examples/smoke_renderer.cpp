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

    crd::renderer::Renderable renderable;
    renderable.vertex_buffer = &vertex_buffer;
    renderable.vertex_count = 3;
    renderable.material_instance_id = 42;
    renderable.variant = variant;

    crd::renderer::Renderer renderer;
    renderer.submit(renderable);

    crd::renderer::FramePlan plan;
    const bool ok = renderer.build_frame({}, *runtime, plan);
    CRD_LOG_INFO(g_log_smoke_renderer, "build_frame={} draw_items={}", ok, plan.draw_items.size());
    if (ok && !plan.draw_items.empty())
    {
        const auto& item = plan.draw_items[0];
        CRD_LOG_INFO(g_log_smoke_renderer, "modules={} descriptors={} push_constants={} vertex_attributes={}",
                     item.handoff.modules.size(), item.handoff.descriptor_bindings.size(),
                     item.handoff.push_constants.size(), item.handoff.vertex_attributes.size());
    }

    crd::log::flush();
    crd::log::shutdown();
    return ok ? 0 : 1;
}
