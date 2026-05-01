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
    void begin() override { ++begin_count; }
    void end() override { ++end_count; }
    void reset() override { ++reset_count; }
    void begin_rendering(const crd::rhi::RenderingInfo& /*info*/) override { ++begin_rendering_count; }
    void end_rendering() override { ++end_rendering_count; }
    void bind_pipeline(crd::rhi::Pipeline& /*pipeline*/) override { ++bind_pipeline_count; }
    void bind_vertex_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/) override
    {
        ++bind_vertex_buffer_count;
    }
    void draw(crd::u32 vertex_count, crd::u32 /*first_vertex*/) override
    {
        ++draw_count;
        last_vertex_count = vertex_count;
    }

    int begin_count = 0;
    int end_count = 0;
    int reset_count = 0;
    int begin_rendering_count = 0;
    int end_rendering_count = 0;
    int bind_pipeline_count = 0;
    int bind_vertex_buffer_count = 0;
    int draw_count = 0;
    crd::u32 last_vertex_count = 0;
};

class FakeResolver final : public crd::renderer::PipelineResolver
{
public:
    explicit FakeResolver(crd::rhi::Pipeline& pipeline) : m_pipeline(pipeline) {}
    [[nodiscard]] crd::rhi::Pipeline*
    resolve_pipeline(const crd::shader::VariantPipelineDesc& /*handoff*/) noexcept override
    {
        ++resolve_count;
        return &m_pipeline;
    }

    int resolve_count = 0;

private:
    crd::rhi::Pipeline& m_pipeline;
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

TEST_CASE("Renderer executes one pass over prepared draw items", "[renderer]")
{
    crd::renderer::FramePlan plan;
    FakeBuffer vertex_buffer(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    plan.draw_items.push_back(
        {crd::math::Mat4f::identity(), crd::math::Mat4f::identity(), &vertex_buffer, 3, 1, {1}, {}});

    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);
    FakeCommandBuffer command_buffer;
    crd::renderer::Renderer renderer;

    REQUIRE(renderer.execute_frame(
        plan,
        {{1280, 720}, {nullptr, crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.1f, 0.1f, 0.1f, 1.0f}}, nullptr},
        command_buffer, resolver));
    REQUIRE(resolver.resolve_count == 1);
    REQUIRE(command_buffer.begin_count == 1);
    REQUIRE(command_buffer.begin_rendering_count == 1);
    REQUIRE(command_buffer.bind_pipeline_count == 1);
    REQUIRE(command_buffer.bind_vertex_buffer_count == 1);
    REQUIRE(command_buffer.draw_count == 1);
    REQUIRE(command_buffer.last_vertex_count == 3u);
    REQUIRE(command_buffer.end_rendering_count == 1);
    REQUIRE(command_buffer.end_count == 1);
}
