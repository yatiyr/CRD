#include <crd/platform/filesystem.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/render_path.hpp>
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

// ---- RHI fakes -------------------------------------------------------

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

// ---- IRenderPath fake -----------------------------------------------

class FakeRenderPath final : public crd::renderer::IRenderPath
{
public:
    void build(crd::renderer::FrameGraph& /*fg*/, const crd::renderer::DrawList& draw_list,
               const crd::renderer::FrameContext& /*ctx*/) override
    {
        ++build_count;
        last_opaque_count = static_cast<int>(draw_list.opaque.size());
        last_masked_count = static_cast<int>(draw_list.masked.size());
        last_translucent_count = static_cast<int>(draw_list.translucent.size());
        last_total_count = static_cast<int>(draw_list.total_count());
    }

    [[nodiscard]] crd::renderer::ImageHandle output_image() const noexcept override { return {}; }

    void resize(crd::rhi::Extent2D new_extent) override
    {
        ++resize_count;
        last_resize_extent = new_extent;
    }

    int build_count = 0;
    int last_opaque_count = 0;
    int last_masked_count = 0;
    int last_translucent_count = 0;
    int last_total_count = 0;
    int resize_count = 0;
    crd::rhi::Extent2D last_resize_extent{};
};

// ---- Shared shader setup helper ------------------------------------

struct ShaderFixture
{
    std::unique_ptr<crd::shader::Runtime> runtime;
    crd::shader::VariantHandle variant;

    ShaderFixture()
    {
        runtime = crd::shader::create_runtime();
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
        variant = runtime->request_variant(request, diagnostics);
    }
};

// ---- Renderable factory --------------------------------------------

[[nodiscard]] crd::renderer::Renderable make_renderable(crd::rhi::Buffer& vb,
                                                         crd::shader::VariantHandle variant,
                                                         crd::renderer::DrawBucket bucket,
                                                         crd::math::Vec3f translation = {})
{
    crd::renderer::Renderable r;
    r.transform.translation = translation;
    r.vertex_buffer = &vb;
    r.vertex_count = 3;
    r.material_instance_id = 0;
    r.variant = variant;
    r.bucket = bucket;
    return r;
}
} // namespace

// =============================================================================
// Renderer tests
// =============================================================================

TEST_CASE("Renderer builds DrawList from renderables and shader handoff", "[renderer]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vertex_buffer(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;
    crd::renderer::Renderable renderable = make_renderable(vertex_buffer, fx.variant, crd::renderer::DrawBucket::Opaque);
    renderable.material_instance_id = 7;
    renderer.submit(renderable);

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    REQUIRE(draw_list.opaque.size() == 1u);
    REQUIRE(draw_list.masked.empty());
    REQUIRE(draw_list.translucent.empty());
    REQUIRE(draw_list.opaque[0].vertex_count == 3u);
    REQUIRE(draw_list.opaque[0].material_instance_id == 7u);
    REQUIRE(draw_list.opaque[0].handoff.modules.size() == 2u);
    REQUIRE(draw_list.opaque[0].handoff.descriptor_bindings.size() == 2u);
}

TEST_CASE("Renderer routes renderables to correct DrawList buckets", "[renderer]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vb(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Masked));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Translucent));

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    REQUIRE(draw_list.opaque.size() == 2u);
    REQUIRE(draw_list.masked.size() == 1u);
    REQUIRE(draw_list.translucent.size() == 1u);
    REQUIRE(draw_list.total_count() == 4u);
    REQUIRE_FALSE(draw_list.empty());
}

TEST_CASE("Renderer sorts opaque front-to-back and translucent back-to-front", "[renderer]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vb(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;

    // Submit far opaque first, near second — build_frame must reorder to front-to-back.
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque, {0.f, 0.f, 10.f}));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque, {0.f, 0.f,  2.f}));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque, {0.f, 0.f,  5.f}));

    // Submit near translucent first — build_frame must reorder to back-to-front.
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Translucent, {0.f, 0.f, 1.f}));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Translucent, {0.f, 0.f, 8.f}));

    crd::renderer::FrameContext ctx;
    ctx.camera_position = {0.f, 0.f, 0.f};

    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    REQUIRE(draw_list.opaque.size() == 3u);
    // Opaque: ascending depth (front-to-back). d^2: 4, 25, 100.
    REQUIRE(draw_list.opaque[0].depth < draw_list.opaque[1].depth);
    REQUIRE(draw_list.opaque[1].depth < draw_list.opaque[2].depth);

    REQUIRE(draw_list.translucent.size() == 2u);
    // Translucent: descending depth (back-to-front). d^2: 64, 1.
    REQUIRE(draw_list.translucent[0].depth > draw_list.translucent[1].depth);
}

TEST_CASE("Renderer build_frame returns false for invalid renderables", "[renderer]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    crd::renderer::Renderer renderer;

    // Null vertex buffer
    crd::renderer::Renderable bad;
    bad.vertex_buffer = nullptr;
    bad.vertex_count = 3;
    bad.variant = fx.variant;
    renderer.submit(bad);

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE_FALSE(renderer.build_frame(ctx, *fx.runtime, draw_list));
}

TEST_CASE("IRenderPath build receives draw list counts from Renderer", "[renderer][render_path]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vb(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Translucent));

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    crd::renderer::FrameGraph fg;
    FakeRenderPath path;
    path.build(fg, draw_list, ctx);

    REQUIRE(path.build_count == 1);
    REQUIRE(path.last_opaque_count == 2);
    REQUIRE(path.last_masked_count == 0);
    REQUIRE(path.last_translucent_count == 1);
    REQUIRE(path.last_total_count == 3);
}

TEST_CASE("IRenderPath resize notifies path of new viewport extent", "[renderer][render_path]")
{
    FakeRenderPath path;
    REQUIRE(path.resize_count == 0);

    path.resize({1920, 1080});
    REQUIRE(path.resize_count == 1);
    REQUIRE(path.last_resize_extent.width == 1920u);
    REQUIRE(path.last_resize_extent.height == 1080u);

    path.resize({2560, 1440});
    REQUIRE(path.resize_count == 2);
    REQUIRE(path.last_resize_extent.width == 2560u);
    REQUIRE(path.last_resize_extent.height == 1440u);
}

TEST_CASE("DrawList clear empties all buckets", "[renderer]")
{
    crd::renderer::DrawList draw_list;
    crd::renderer::DrawItem item;
    draw_list.opaque.push_back(item);
    draw_list.masked.push_back(item);
    draw_list.translucent.push_back(item);

    REQUIRE(draw_list.total_count() == 3u);
    REQUIRE_FALSE(draw_list.empty());

    draw_list.clear();

    REQUIRE(draw_list.total_count() == 0u);
    REQUIRE(draw_list.empty());
}

// =============================================================================
// FrameGraph tests (unchanged from v1c)
// =============================================================================

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

    REQUIRE(depth_prepass_order == 0);
    REQUIRE(main_color_order == 1);

    REQUIRE(cmd.transition_count == 2);
    REQUIRE(cmd.transitions[0].from == crd::rhi::ImageAccess::Undefined);
    REQUIRE(cmd.transitions[0].to == crd::rhi::ImageAccess::DepthWrite);
    REQUIRE(cmd.transitions[1].from == crd::rhi::ImageAccess::DepthWrite);
    REQUIRE(cmd.transitions[1].to == crd::rhi::ImageAccess::DepthRead);
}

TEST_CASE("FrameGraph build fails when a transient image is read before being written",
          "[renderer][frame_graph]")
{
    crd::renderer::FrameGraph fg;

    auto transient = fg.create_transient({{1280, 720}, crd::rhi::Format::B8G8R8A8Unorm,
                                          crd::rhi::enum_bits(crd::rhi::ImageUsage::ColorAttachment)});

    auto builder = fg.add_pass("bad-pass");
    builder.read(transient, crd::rhi::ImageAccess::ShaderRead);

    REQUIRE_FALSE(fg.build());
}

TEST_CASE("FrameGraph no-op barrier when access doesn't change between passes", "[renderer][frame_graph]")
{
    FakeImage image({});
    crd::renderer::FrameGraph fg;

    auto handle = fg.import(&image, crd::rhi::ImageAccess::ColorWrite);

    auto builder = fg.add_pass("pass-a");
    builder.write(handle, crd::rhi::ImageAccess::ColorWrite);
    builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});

    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    FakeDevice fake_device;
    fg.execute(fake_device, cmd);

    REQUIRE(cmd.transition_count == 0);
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

    auto handle2 = fg.import(&image, crd::rhi::ImageAccess::Undefined);
    REQUIRE(handle2.index == 0u);
    auto builder2 = fg.add_pass("pass-after-reset");
    builder2.write(handle2, crd::rhi::ImageAccess::ColorWrite);
    builder2.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    REQUIRE(fg.build());
}
