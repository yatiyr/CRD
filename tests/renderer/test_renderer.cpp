#include <crd/platform/filesystem.hpp>
#include <crd/renderer/frame_graph.hpp>
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

struct TransitionRecord
{
    crd::rhi::ImageAccess from;
    crd::rhi::ImageAccess to;
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
    void transition_image(crd::rhi::Image& /*image*/, crd::rhi::ImageAccess from,
                          crd::rhi::ImageAccess to) noexcept override
    {
        ++transition_count;
        transitions.push_back({from, to});
    }

    int begin_count = 0;
    int end_count = 0;
    int reset_count = 0;
    int begin_rendering_count = 0;
    int end_rendering_count = 0;
    int bind_pipeline_count = 0;
    int bind_vertex_buffer_count = 0;
    int draw_count = 0;
    int transition_count = 0;
    crd::u32 last_vertex_count = 0;
    crd::containers::Array<TransitionRecord> transitions{};
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

class FakeQueue final : public crd::rhi::Queue
{
public:
    bool submit(crd::rhi::CommandBuffer& /*cmd*/, crd::rhi::Swapchain& /*sc*/) override { return true; }
    void present(crd::rhi::Swapchain& /*sc*/) override {}
    void wait_idle() override {}
};

class FakeDevice final : public crd::rhi::Device
{
public:
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

private:
    FakeQueue m_queue{};
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

TEST_CASE("FrameGraph build succeeds with a single pass writing an external image", "[renderer][frame_graph]")
{
    FakeImage color_image({});
    crd::renderer::FrameGraph fg;

    auto color = fg.import(&color_image, crd::rhi::ImageAccess::Undefined);
    REQUIRE(color.is_valid());

    bool pass_executed = false;
    auto builder = fg.add_pass("main-color");
    builder.write(color, crd::rhi::ImageAccess::ColorWrite);
    builder.set_execute([&](crd::renderer::FrameResources& res, crd::rhi::CommandBuffer& /*cmd*/) {
        pass_executed = true;
        REQUIRE(res.get(color) == &color_image);
    });

    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    FakeDevice fake_device;
    fg.execute(fake_device, cmd);

    REQUIRE(pass_executed);
    REQUIRE(cmd.transition_count == 1);
    REQUIRE(cmd.transitions[0].from == crd::rhi::ImageAccess::Undefined);
    REQUIRE(cmd.transitions[0].to == crd::rhi::ImageAccess::ColorWrite);
}

TEST_CASE("FrameGraph inserts barrier between two passes sharing a resource", "[renderer][frame_graph]")
{
    FakeImage depth_image({});
    crd::renderer::FrameGraph fg;

    auto depth = fg.import(&depth_image, crd::rhi::ImageAccess::Undefined);

    int pass_order = 0;
    int depth_prepass_order = -1;
    int main_color_order = -1;

    {
        auto builder = fg.add_pass("depth-prepass");
        builder.write(depth, crd::rhi::ImageAccess::DepthWrite);
        builder.set_execute([&](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {
            depth_prepass_order = pass_order++;
        });
    }
    {
        auto builder = fg.add_pass("main-color");
        builder.read(depth, crd::rhi::ImageAccess::DepthRead);
        builder.set_execute([&](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {
            main_color_order = pass_order++;
        });
    }

    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    FakeDevice fake_device;
    fg.execute(fake_device, cmd);

    // Passes run in declaration order
    REQUIRE(depth_prepass_order == 0);
    REQUIRE(main_color_order == 1);

    // Two transitions: Undefined→DepthWrite (before depth-prepass), DepthWrite→DepthRead (before main-color)
    REQUIRE(cmd.transition_count == 2);
    REQUIRE(cmd.transitions[0].from == crd::rhi::ImageAccess::Undefined);
    REQUIRE(cmd.transitions[0].to == crd::rhi::ImageAccess::DepthWrite);
    REQUIRE(cmd.transitions[1].from == crd::rhi::ImageAccess::DepthWrite);
    REQUIRE(cmd.transitions[1].to == crd::rhi::ImageAccess::DepthRead);
}

TEST_CASE("FrameGraph build fails when a transient image is read before being written", "[renderer][frame_graph]")
{
    crd::renderer::FrameGraph fg;

    auto transient = fg.create_transient({{1280, 720}, crd::rhi::Format::B8G8R8A8Unorm,
                                          crd::rhi::enum_bits(crd::rhi::ImageUsage::ColorAttachment)});

    auto builder = fg.add_pass("bad-pass");
    builder.read(transient, crd::rhi::ImageAccess::ShaderRead); // read before any write — invalid

    REQUIRE_FALSE(fg.build());
}

TEST_CASE("FrameGraph no-op barrier when access doesn't change between passes", "[renderer][frame_graph]")
{
    FakeImage image({});
    crd::renderer::FrameGraph fg;

    auto handle = fg.import(&image, crd::rhi::ImageAccess::ColorWrite); // already in ColorWrite

    auto builder = fg.add_pass("pass-a");
    builder.write(handle, crd::rhi::ImageAccess::ColorWrite); // same access — no barrier needed
    builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});

    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    FakeDevice fake_device;
    fg.execute(fake_device, cmd);

    REQUIRE(cmd.transition_count == 0); // no transition needed
}

TEST_CASE("FrameGraph reset clears passes and resources", "[renderer][frame_graph]")
{
    FakeImage image({});
    crd::renderer::FrameGraph fg;

    auto handle = fg.import(&image, crd::rhi::ImageAccess::Undefined);
    auto builder = fg.add_pass("pass");
    builder.write(handle, crd::rhi::ImageAccess::ColorWrite);
    builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    REQUIRE(fg.build());

    fg.reset();

    // After reset, re-import and re-declare: handle indices restart from 0
    auto handle2 = fg.import(&image, crd::rhi::ImageAccess::Undefined);
    REQUIRE(handle2.index == 0u); // fresh index after reset
    auto builder2 = fg.add_pass("pass-after-reset");
    builder2.write(handle2, crd::rhi::ImageAccess::ColorWrite);
    builder2.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    REQUIRE(fg.build()); // must build cleanly after reset
}
