#include <crd/platform/filesystem.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] crd::containers::String source_path(const char* relative)
{
    return crd::containers::String((fs::Path(CRD_SOURCE_DIR) / relative).generic().data());
}

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

TEST_CASE("Renderer builds explicit draw items from renderables and shader handoff", "[renderer]")
{
    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("renderer_case");
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/reflect_triangle.vert"),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/reflect_triangle.frag"),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);
    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE(variant.is_valid());

    crd::renderer::Renderer renderer;
    FakeBuffer vertex_buffer(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderable renderable;
    renderable.vertex_buffer = &vertex_buffer;
    renderable.vertex_count = 3;
    renderable.material_instance_id = 7;
    renderable.variant = variant;
    renderer.submit(renderable);

    crd::renderer::FramePlan plan;
    REQUIRE(renderer.build_frame({}, *runtime, plan));
    REQUIRE(plan.draw_items.size() == 1u);
    REQUIRE(plan.draw_items[0].vertex_count == 3u);
    REQUIRE(plan.draw_items[0].material_instance_id == 7u);
    REQUIRE(plan.draw_items[0].handoff.modules.size() == 2u);
    REQUIRE(plan.draw_items[0].handoff.descriptor_bindings.size() == 2u);
}
