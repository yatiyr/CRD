#include <crd/platform/filesystem.hpp>
#include <crd/renderer/forward_render_path.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/material.hpp>
#include <crd/renderer/per_frame_data.hpp>
#include <crd/renderer/render_path.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/renderer/swapchain_blit.hpp>
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
    void bind_index_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/,
                           crd::rhi::IndexType type) override
    {
        ++bind_index_buffer_count;
        last_index_type = type;
    }
    void draw(crd::u32 vertex_count, crd::u32 /*first_vertex*/) override
    {
        ++draw_count;
        last_vertex_count = vertex_count;
    }
    void draw_indexed(crd::u32 index_count, crd::u32 /*first_index*/, crd::i32 /*vertex_offset*/) override
    {
        ++draw_indexed_count;
        last_index_count = index_count;
    }
    void blit_image(crd::rhi::Image& /*src*/, crd::rhi::Image& /*dst*/,
                    crd::rhi::Extent2D src_extent, crd::rhi::Extent2D /*dst_extent*/) noexcept override
    {
        ++blit_image_count;
        last_blit_src_extent = src_extent;
    }
    void transition_image(crd::rhi::Image& /*image*/, crd::rhi::ImageAccess from,
                          crd::rhi::ImageAccess to) noexcept override
    {
        ++transition_count;
        transitions.push_back({from, to});
    }
    void push_constants(crd::rhi::PipelineLayout& /*layout*/, crd::rhi::ShaderStage /*stages*/,
                        crd::u32 /*offset*/, crd::u32 size, const void* /*data*/) override
    {
        ++push_constants_count;
        last_push_size = size;
    }
    void bind_descriptor_sets(crd::rhi::PipelineLayout& /*layout*/, crd::u32 first_set,
                              crd::containers::ConstSpan<crd::rhi::DescriptorSet*> sets) override
    {
        ++bind_descriptor_sets_count;
        last_first_set = first_set;
        last_set_count = static_cast<int>(sets.size());
    }

    int begin_count = 0;
    int end_count = 0;
    int reset_count = 0;
    int begin_rendering_count = 0;
    int end_rendering_count = 0;
    int bind_pipeline_count = 0;
    int bind_vertex_buffer_count = 0;
    int bind_index_buffer_count = 0;
    int draw_count = 0;
    int draw_indexed_count = 0;
    int blit_image_count = 0;
    int transition_count = 0;
    int push_constants_count = 0;
    int bind_descriptor_sets_count = 0;
    crd::u32 last_vertex_count = 0;
    crd::u32 last_index_count = 0;
    crd::rhi::IndexType last_index_type = crd::rhi::IndexType::Uint32;
    crd::u32 last_push_size = 0;
    crd::u32 last_first_set = 0;
    int last_set_count = 0;
    crd::rhi::Extent2D last_blit_src_extent{};
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

class FakeDescriptorSetLayout final : public crd::rhi::DescriptorSetLayout
{
public:
    explicit FakeDescriptorSetLayout(crd::rhi::DescriptorSetLayoutDesc desc) : m_desc(std::move(desc)) {}
    [[nodiscard]] const crd::rhi::DescriptorSetLayoutDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::DescriptorSetLayoutDesc m_desc{};
};

class FakePipelineLayout final : public crd::rhi::PipelineLayout
{
public:
    explicit FakePipelineLayout(crd::rhi::PipelineLayoutDesc desc) : m_desc(std::move(desc)) {}
    [[nodiscard]] const crd::rhi::PipelineLayoutDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::PipelineLayoutDesc m_desc{};
};

class FakeDescriptorSet final : public crd::rhi::DescriptorSet
{
public:
    void update_buffer(crd::u32 binding, crd::rhi::Buffer& /*buf*/,
                       crd::u64 /*off*/, crd::u64 /*sz*/) override
    {
        ++update_count;
        last_binding = binding;
    }

    int update_count  = 0;
    crd::u32 last_binding = 0;
};

class FakeDescriptorAllocator final : public crd::rhi::DescriptorAllocator
{
public:
    void begin_frame(crd::u32 frame_index) override
    {
        ++begin_frame_count;
        last_frame = frame_index;
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorSet>
    allocate(const crd::rhi::DescriptorSetLayout& layout) override
    {
        ++allocate_count;
        last_binding_count = static_cast<int>(layout.desc().bindings.size());
        return std::make_unique<FakeDescriptorSet>();
    }

    int begin_frame_count = 0;
    int allocate_count    = 0;
    crd::u32 last_frame   = 0;
    int last_binding_count = 0;
};

class FakeDevice final : public crd::rhi::Device
{
public:
    [[nodiscard]] crd::rhi::BackendApi api() const noexcept override { return crd::rhi::BackendApi::Vulkan; }
    [[nodiscard]] std::unique_ptr<crd::rhi::Swapchain> create_swapchain(const crd::rhi::SwapchainDesc&) override
    {
        return nullptr;
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

    [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorSetLayout>
    create_descriptor_set_layout(const crd::rhi::DescriptorSetLayoutDesc& desc) override
    {
        ++create_dsl_count;
        return std::make_unique<FakeDescriptorSetLayout>(desc);
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::PipelineLayout>
    create_pipeline_layout(const crd::rhi::PipelineLayoutDesc& desc) override
    {
        ++create_pl_count;
        return std::make_unique<FakePipelineLayout>(desc);
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorAllocator>
    create_descriptor_allocator(const crd::rhi::DescriptorAllocatorDesc&) override
    {
        ++create_alloc_count;
        return std::make_unique<FakeDescriptorAllocator>();
    }

    [[nodiscard]] crd::rhi::Queue& graphics_queue() noexcept override { return m_queue; }
    void wait_idle() override {}

    int create_dsl_count    = 0;
    int create_pl_count     = 0;
    int create_alloc_count  = 0;
    int create_buffer_count = 0;
    int create_image_count  = 0;

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
        {sizeof(float) * 15U, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;
    crd::renderer::Renderable renderable = make_renderable(vertex_buffer, fx.variant, crd::renderer::DrawBucket::Opaque);
    renderable.material_instance_id = 7;
    renderer.submit(renderable);

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    REQUIRE(draw_list.opaque.size() == 1U);
    REQUIRE(draw_list.masked.empty());
    REQUIRE(draw_list.translucent.empty());
    REQUIRE(draw_list.opaque[0].vertex_count == 3U);
    REQUIRE(draw_list.opaque[0].material_instance_id == 7U);
    REQUIRE(draw_list.opaque[0].handoff.modules.size() == 2U);
    REQUIRE(draw_list.opaque[0].handoff.descriptor_bindings.size() == 2U);
}

TEST_CASE("Renderer routes renderables to correct DrawList buckets", "[renderer]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vb(
        {sizeof(float) * 15U, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Masked));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Translucent));

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    REQUIRE(draw_list.opaque.size() == 2U);
    REQUIRE(draw_list.masked.size() == 1U);
    REQUIRE(draw_list.translucent.size() == 1U);
    REQUIRE(draw_list.total_count() == 4U);
    REQUIRE_FALSE(draw_list.empty());
}

TEST_CASE("Renderer sorts opaque front-to-back and translucent back-to-front", "[renderer]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vb(
        {sizeof(float) * 15U, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;

    // Submit far opaque first, near second — build_frame must reorder to front-to-back.
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque, {0.F, 0.F, 10.F}));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque, {0.F, 0.F,  2.F}));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque, {0.F, 0.F,  5.F}));

    // Submit near translucent first — build_frame must reorder to back-to-front.
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Translucent, {0.F, 0.F, 1.F}));
    renderer.submit(make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Translucent, {0.F, 0.F, 8.F}));

    crd::renderer::FrameContext ctx;
    ctx.camera_position = {0.F, 0.F, 0.F};

    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    REQUIRE(draw_list.opaque.size() == 3U);
    // Opaque: ascending depth (front-to-back). d^2: 4, 25, 100.
    REQUIRE(draw_list.opaque[0].depth < draw_list.opaque[1].depth);
    REQUIRE(draw_list.opaque[1].depth < draw_list.opaque[2].depth);

    REQUIRE(draw_list.translucent.size() == 2U);
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
        {sizeof(float) * 15U, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

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
    REQUIRE(path.last_resize_extent.width == 1920U);
    REQUIRE(path.last_resize_extent.height == 1080U);

    path.resize({2560, 1440});
    REQUIRE(path.resize_count == 2);
    REQUIRE(path.last_resize_extent.width == 2560U);
    REQUIRE(path.last_resize_extent.height == 1440U);
}

TEST_CASE("DrawList clear empties all buckets", "[renderer]")
{
    crd::renderer::DrawList draw_list;
    crd::renderer::DrawItem item;
    draw_list.opaque.push_back(item);
    draw_list.masked.push_back(item);
    draw_list.translucent.push_back(item);

    REQUIRE(draw_list.total_count() == 3U);
    REQUIRE_FALSE(draw_list.empty());

    draw_list.clear();

    REQUIRE(draw_list.total_count() == 0U);
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
    REQUIRE(handle2.index == 0U);
    auto builder2 = fg.add_pass("pass-after-reset");
    builder2.write(handle2, crd::rhi::ImageAccess::ColorWrite);
    builder2.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    REQUIRE(fg.build());
}

// =============================================================================
// Material system tests (v1e+f)
// =============================================================================

TEST_CASE("MaterialBindLayout creates a DescriptorSetLayout on the device", "[renderer][material]")
{
    FakeDevice device;

    crd::rhi::DescriptorBinding bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Fragment},
    };
    auto layout = crd::renderer::MaterialBindLayout::create(device, {crd::containers::make_span(bindings)});

    REQUIRE(layout != nullptr);
    REQUIRE(device.create_dsl_count == 1);
    REQUIRE(layout->descriptor_set_layout().desc().bindings.size() == 1U);
    REQUIRE(layout->descriptor_set_layout().desc().bindings[0].type
            == crd::rhi::DescriptorType::UniformBuffer);
}

TEST_CASE("MaterialBindGroup allocates a DescriptorSet from the ring allocator", "[renderer][material]")
{
    FakeDevice device;
    FakeDescriptorAllocator allocator;

    crd::rhi::DescriptorBinding bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Fragment},
        {1, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Fragment},
    };
    auto layout = crd::renderer::MaterialBindLayout::create(device, {crd::containers::make_span(bindings)});
    REQUIRE(layout != nullptr);

    allocator.begin_frame(0);
    auto instance = layout->create_instance(allocator);

    REQUIRE(instance != nullptr);
    REQUIRE(allocator.allocate_count == 1);
    REQUIRE(allocator.last_binding_count == 2);
}

TEST_CASE("MaterialBindGroup update_buffer forwards to the underlying DescriptorSet", "[renderer][material]")
{
    FakeDevice device;

    crd::rhi::DescriptorBinding bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Fragment},
    };
    auto layout = crd::renderer::MaterialBindLayout::create(device, {crd::containers::make_span(bindings)});

    // Build a MaterialBindGroup directly with a FakeDescriptorSet
    auto fake_set = std::make_unique<FakeDescriptorSet>();
    auto* raw_set = fake_set.get();
    crd::renderer::MaterialBindGroup instance(std::move(fake_set));

    FakeBuffer buf({64, crd::rhi::enum_bits(crd::rhi::BufferUsage::Uniform), crd::rhi::MemoryUsage::CpuToGpu});
    instance.update_buffer(0, buf);

    REQUIRE(raw_set->update_count == 1);
    REQUIRE(raw_set->last_binding == 0U);
    REQUIRE(&instance.descriptor_set() == raw_set);
}

TEST_CASE("DescriptorAllocator ring: begin_frame advances frame index", "[renderer][material]")
{
    FakeDevice device;

    crd::rhi::DescriptorBinding bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Vertex},
    };
    auto layout = crd::renderer::MaterialBindLayout::create(device, {crd::containers::make_span(bindings)});
    auto alloc  = device.create_descriptor_allocator({2, 128});
    auto* fake  = static_cast<FakeDescriptorAllocator*>(alloc.get());

    alloc->begin_frame(0);
    auto inst0 = layout->create_instance(*alloc);
    REQUIRE(inst0 != nullptr);

    alloc->begin_frame(1);
    auto inst1 = layout->create_instance(*alloc);
    REQUIRE(inst1 != nullptr);

    REQUIRE(fake->begin_frame_count == 2);
    REQUIRE(fake->allocate_count == 2);
    REQUIRE(fake->last_frame == 1U);
}

// =============================================================================
// ForwardRenderPath tests (v1g)
// =============================================================================

TEST_CASE("ForwardRenderPath create allocates layouts UBOs and render targets", "[renderer][forward]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);

    REQUIRE(frp != nullptr);
    REQUIRE(device.create_dsl_count == 1);   // per-frame set layout
    REQUIRE(device.create_pl_count  == 1);   // pipeline layout
    REQUIRE(device.create_buffer_count == 2); // one UBO per frame-in-flight
    REQUIRE(device.create_image_count  == 2); // color + depth render targets
}

TEST_CASE("ForwardRenderPath build registers two frame graph passes", "[renderer][forward]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);
    REQUIRE(frp != nullptr);

    FakeBuffer vb({12, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    crd::renderer::DrawItem item;
    item.vertex_buffer = &vb;
    item.vertex_count  = 3;

    crd::renderer::DrawList draw_list;
    draw_list.opaque.push_back(item);

    crd::renderer::FrameContext ctx;
    ctx.frame_index = 0;

    crd::renderer::FrameGraph fg;
    alloc.begin_frame(0);
    frp->build(fg, draw_list, ctx);

    REQUIRE(frp->output_image().is_valid());
    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    fg.execute(device, cmd);

    // Two passes: depth-prepass + main-color.
    REQUIRE(cmd.begin_rendering_count == 2);
    REQUIRE(cmd.end_rendering_count == 2);
    // 1 opaque item drawn in each pass.
    REQUIRE(cmd.draw_count == 2);
    // push_constants called once per draw.
    REQUIRE(cmd.push_constants_count == 2);
    // model matrix is 64 bytes.
    REQUIRE(cmd.last_push_size == 64U);
    // bind_descriptor_sets called once in the color pass (set 0).
    REQUIRE(cmd.bind_descriptor_sets_count == 1);
    REQUIRE(cmd.last_first_set == 0U);
}

TEST_CASE("ForwardRenderPath build draws opaque and masked in color pass", "[renderer][forward]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);
    REQUIRE(frp != nullptr);

    FakeBuffer vb({12, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    crd::renderer::DrawItem opaque_item;
    opaque_item.vertex_buffer = &vb;
    opaque_item.vertex_count  = 3;
    crd::renderer::DrawItem masked_item = opaque_item;

    crd::renderer::DrawList draw_list;
    draw_list.opaque.push_back(opaque_item);
    draw_list.masked.push_back(masked_item);

    crd::renderer::FrameContext ctx;
    ctx.frame_index = 0;

    crd::renderer::FrameGraph fg;
    alloc.begin_frame(0);
    frp->build(fg, draw_list, ctx);
    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    fg.execute(device, cmd);

    // depth-prepass draws only opaque (1), color pass draws opaque+masked (2).
    REQUIRE(cmd.draw_count == 3);
}

TEST_CASE("ForwardRenderPath resize recreates render targets", "[renderer][forward]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);
    REQUIRE(frp != nullptr);

    const int images_before = device.create_image_count;

    frp->resize({1920, 1080});
    REQUIRE(device.create_image_count == images_before + 2); // color + depth recreated

    // Resize to the same extent must be a no-op.
    frp->resize({1920, 1080});
    REQUIRE(device.create_image_count == images_before + 2);
}

TEST_CASE("ForwardRenderPath per-frame slot advances correctly across frames", "[renderer][forward]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);
    REQUIRE(frp != nullptr);

    crd::renderer::DrawList empty_list;
    crd::renderer::FrameContext ctx;

    for (crd::u32 frame = 0; frame < 4; ++frame)
    {
        ctx.frame_index = frame;
        alloc.begin_frame(frame);

        crd::renderer::FrameGraph fg;
        frp->build(fg, empty_list, ctx);
        REQUIRE(fg.build());

        FakeCommandBuffer cmd;
        fg.execute(device, cmd);
    }

    // 4 frames × 1 allocate per frame = 4 total allocations.
    REQUIRE(alloc.allocate_count == 4);
}

// =============================================================================
// Index buffer tests (v1h)
// =============================================================================

TEST_CASE("build_frame copies index fields from Renderable into DrawItem", "[renderer][index]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vb({12, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    FakeBuffer ib({24, crd::rhi::enum_bits(crd::rhi::BufferUsage::Index),  crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderable r = make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque);
    r.index_buffer = &ib;
    r.index_count  = 6;
    r.index_type   = crd::rhi::IndexType::Uint16;

    crd::renderer::Renderer renderer;
    renderer.submit(r);

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE(renderer.build_frame(ctx, *fx.runtime, draw_list));

    REQUIRE(draw_list.opaque.size() == 1U);
    const auto& item = draw_list.opaque[0];
    REQUIRE(item.index_buffer == &ib);
    REQUIRE(item.index_count  == 6U);
    REQUIRE(item.index_type   == crd::rhi::IndexType::Uint16);
}

TEST_CASE("build_frame rejects indexed renderable with zero index_count", "[renderer][index]")
{
    ShaderFixture fx;
    REQUIRE(fx.variant.is_valid());

    FakeBuffer vb({12, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    FakeBuffer ib({24, crd::rhi::enum_bits(crd::rhi::BufferUsage::Index),  crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderable r = make_renderable(vb, fx.variant, crd::renderer::DrawBucket::Opaque);
    r.index_buffer = &ib;
    r.index_count  = 0; // invalid: index buffer set but count is zero

    crd::renderer::Renderer renderer;
    renderer.submit(r);

    crd::renderer::FrameContext ctx;
    crd::renderer::DrawList draw_list;
    REQUIRE_FALSE(renderer.build_frame(ctx, *fx.runtime, draw_list));
}

TEST_CASE("ForwardRenderPath dispatches draw_indexed for indexed items", "[renderer][forward][index]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);
    REQUIRE(frp != nullptr);

    FakeBuffer vb({12,  crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    FakeBuffer ib({24,  crd::rhi::enum_bits(crd::rhi::BufferUsage::Index),  crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::DrawItem item;
    item.vertex_buffer = &vb;
    item.vertex_count  = 3;
    item.index_buffer  = &ib;
    item.index_count   = 6;
    item.index_type    = crd::rhi::IndexType::Uint32;

    crd::renderer::DrawList draw_list;
    draw_list.opaque.push_back(item);

    crd::renderer::FrameContext ctx;
    ctx.frame_index = 0;

    crd::renderer::FrameGraph fg;
    alloc.begin_frame(0);
    frp->build(fg, draw_list, ctx);
    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    fg.execute(device, cmd);

    // Indexed item: draw_indexed in depth-prepass + color pass; draw never called.
    REQUIRE(cmd.draw_count         == 0);
    REQUIRE(cmd.draw_indexed_count == 2);
    REQUIRE(cmd.bind_index_buffer_count == 2);
    REQUIRE(cmd.last_index_count   == 6U);
    REQUIRE(cmd.last_index_type    == crd::rhi::IndexType::Uint32);
}

TEST_CASE("ForwardRenderPath mixes indexed and non-indexed items in color pass", "[renderer][forward][index]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);
    REQUIRE(frp != nullptr);

    FakeBuffer vb({12, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    FakeBuffer ib({24, crd::rhi::enum_bits(crd::rhi::BufferUsage::Index),  crd::rhi::MemoryUsage::CpuToGpu});

    // One indexed opaque, one non-indexed masked.
    crd::renderer::DrawItem indexed_item;
    indexed_item.vertex_buffer = &vb;
    indexed_item.vertex_count  = 3;
    indexed_item.index_buffer  = &ib;
    indexed_item.index_count   = 6;

    crd::renderer::DrawItem plain_item;
    plain_item.vertex_buffer = &vb;
    plain_item.vertex_count  = 3;

    crd::renderer::DrawList draw_list;
    draw_list.opaque.push_back(indexed_item);
    draw_list.masked.push_back(plain_item);

    crd::renderer::FrameContext ctx;
    ctx.frame_index = 0;

    crd::renderer::FrameGraph fg;
    alloc.begin_frame(0);
    frp->build(fg, draw_list, ctx);
    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    fg.execute(device, cmd);

    // depth-prepass: 1 indexed opaque → draw_indexed × 1
    // color pass:    1 indexed opaque + 1 plain masked → draw_indexed × 1 + draw × 1
    REQUIRE(cmd.draw_indexed_count == 2);
    REQUIRE(cmd.draw_count         == 1);
}

// =============================================================================
// Swapchain blit tests (v1i)
// =============================================================================

TEST_CASE("add_swapchain_blit_pass imports swapchain and adds two passes", "[renderer][blit]")
{
    FakeImage render_target({});
    FakeImage swapchain_image({});

    crd::renderer::FrameGraph fg;
    auto rt_handle = fg.import(&render_target, crd::rhi::ImageAccess::ColorWrite);

    const crd::rhi::Extent2D render_ext{1280, 720};
    const crd::rhi::Extent2D display_ext{1280, 720};

    auto sc_handle = crd::renderer::add_swapchain_blit_pass(fg, rt_handle, swapchain_image,
                                                             render_ext, display_ext);

    REQUIRE(sc_handle.is_valid());
    REQUIRE(fg.build()); // build must succeed: external rt is pre-written, swapchain external
}

TEST_CASE("add_swapchain_blit_pass execute calls blit_image once", "[renderer][blit]")
{
    FakeImage render_target({});
    FakeImage swapchain_image({});
    FakeDevice fake_device;

    crd::renderer::FrameGraph fg;
    // Render output starts as ColorWrite (post-ForwardRenderPath).
    auto rt_handle = fg.import(&render_target, crd::rhi::ImageAccess::ColorWrite);

    const crd::rhi::Extent2D render_ext{1280, 720};
    const crd::rhi::Extent2D display_ext{1920, 1080};

    [[maybe_unused]] auto sc_h = crd::renderer::add_swapchain_blit_pass(fg, rt_handle, swapchain_image, render_ext, display_ext);

    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    fg.execute(fake_device, cmd);

    REQUIRE(cmd.blit_image_count == 1);
    REQUIRE(cmd.last_blit_src_extent.width  == 1280U);
    REQUIRE(cmd.last_blit_src_extent.height == 720U);
}

TEST_CASE("add_swapchain_blit_pass inserts correct barrier sequence", "[renderer][blit]")
{
    // Verify barrier sequence:
    //   ColorWrite → TransferSrc (render output, before swapchain-blit)
    //   Undefined  → TransferDst (swapchain image, before swapchain-blit)
    //   TransferDst → Present    (swapchain image, before present-barrier)
    FakeImage render_target({});
    FakeImage swapchain_image({});
    FakeDevice fake_device;

    crd::renderer::FrameGraph fg;
    auto rt_handle = fg.import(&render_target, crd::rhi::ImageAccess::ColorWrite);

    [[maybe_unused]] auto sc_h2 = crd::renderer::add_swapchain_blit_pass(fg, rt_handle, swapchain_image, {1280, 720}, {1280, 720});

    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    fg.execute(fake_device, cmd);

    // 3 barriers: ColorWrite→TransferSrc, Undefined→TransferDst, TransferDst→Present
    REQUIRE(cmd.transition_count == 3);
    REQUIRE(cmd.transitions[0].from == crd::rhi::ImageAccess::ColorWrite);
    REQUIRE(cmd.transitions[0].to   == crd::rhi::ImageAccess::TransferSrc);
    REQUIRE(cmd.transitions[1].from == crd::rhi::ImageAccess::Undefined);
    REQUIRE(cmd.transitions[1].to   == crd::rhi::ImageAccess::TransferDst);
    REQUIRE(cmd.transitions[2].from == crd::rhi::ImageAccess::TransferDst);
    REQUIRE(cmd.transitions[2].to   == crd::rhi::ImageAccess::Present);
}

TEST_CASE("add_swapchain_blit_pass appended after ForwardRenderPath passes builds correctly",
          "[renderer][blit][forward]")
{
    FakeDevice device;
    FakeDescriptorAllocator alloc;
    FakePipeline pipeline({});
    FakeResolver resolver(pipeline);

    auto frp = crd::renderer::ForwardRenderPath::create(device, resolver, alloc, {1280, 720}, 2);
    REQUIRE(frp != nullptr);

    crd::renderer::DrawList empty_list;
    crd::renderer::FrameContext ctx;
    ctx.frame_index = 0;

    FakeImage swapchain_image({});
    crd::renderer::FrameGraph fg;
    alloc.begin_frame(0);
    frp->build(fg, empty_list, ctx);
    [[maybe_unused]] auto sc_h3 = crd::renderer::add_swapchain_blit_pass(fg, frp->output_image(), swapchain_image,
                                                                          {1280, 720}, {1280, 720});

    REQUIRE(fg.build());

    FakeCommandBuffer cmd;
    fg.execute(device, cmd);

    // blit must have been called exactly once
    REQUIRE(cmd.blit_image_count == 1);
    // swapchain image ends in Present layout: last two transitions on the swapchain are
    // Undefined→TransferDst and TransferDst→Present
    const auto& t = cmd.transitions;
    REQUIRE(t.back().to == crd::rhi::ImageAccess::Present);
}
