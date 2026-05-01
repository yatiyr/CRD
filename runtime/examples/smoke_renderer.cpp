#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/render_path.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/renderer/swapchain_blit.hpp>
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
    void bind_index_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/,
                           crd::rhi::IndexType /*type*/) override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "bind_index_buffer");
    }
    void draw(crd::u32 vertex_count, crd::u32 /*first_vertex*/) override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "draw vertices={}", vertex_count);
    }
    void draw_indexed(crd::u32 index_count, crd::u32 /*first_index*/, crd::i32 /*vertex_offset*/) override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "draw_indexed indices={}", index_count);
    }
    void blit_image(crd::rhi::Image& /*src*/, crd::rhi::Image& /*dst*/,
                    crd::rhi::Extent2D src_ext, crd::rhi::Extent2D dst_ext) noexcept override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "blit_image {}x{} → {}x{}",
                     src_ext.width, src_ext.height, dst_ext.width, dst_ext.height);
        ++blit_count;
    }
    void transition_image(crd::rhi::Image& /*image*/, crd::rhi::ImageAccess from,
                          crd::rhi::ImageAccess to) noexcept override
    {
        CRD_LOG_INFO(g_log_smoke_renderer, "transition_image from={} to={}", static_cast<int>(from),
                     static_cast<int>(to));
        ++transition_count;
    }
    void push_constants(crd::rhi::PipelineLayout& /*layout*/, crd::rhi::ShaderStage /*stages*/,
                        crd::u32 /*offset*/, crd::u32 /*size*/, const void* /*data*/) override
    {
    }
    void bind_descriptor_sets(crd::rhi::PipelineLayout& /*layout*/, crd::u32 /*first_set*/,
                              crd::containers::ConstSpan<crd::rhi::DescriptorSet*> /*sets*/) override
    {
    }

    int transition_count = 0;
    int blit_count = 0;
};

// Minimal IRenderPath implementation that records one color pass into the frame graph.
class SimpleRenderPath final : public crd::renderer::IRenderPath
{
public:
    explicit SimpleRenderPath(crd::rhi::Image& color_target) : m_color_target(color_target) {}

    void build(crd::renderer::FrameGraph& fg, const crd::renderer::DrawList& draw_list,
               const crd::renderer::FrameContext& /*ctx*/) override
    {
        ++m_build_count;
        m_last_opaque      = static_cast<int>(draw_list.opaque.size());
        m_last_masked      = static_cast<int>(draw_list.masked.size());
        m_last_translucent = static_cast<int>(draw_list.translucent.size());

        auto color_h  = fg.import(&m_color_target, crd::rhi::ImageAccess::Undefined);
        m_output      = color_h;

        auto builder = fg.add_pass("smoke-color");
        builder.write(color_h, crd::rhi::ImageAccess::ColorWrite);
        builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    }

    [[nodiscard]] crd::renderer::ImageHandle output_image() const noexcept override { return m_output; }

    void resize(crd::rhi::Extent2D new_extent) override
    {
        m_last_extent = new_extent;
        ++m_resize_count;
    }

    int m_build_count      = 0;
    int m_last_opaque      = 0;
    int m_last_masked      = 0;
    int m_last_translucent = 0;
    int m_resize_count     = 0;
    crd::rhi::Extent2D m_last_extent{};

private:
    crd::rhi::Image&          m_color_target;
    crd::renderer::ImageHandle m_output{};
};
} // namespace

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    // --- Shared fake device (used by both frame graph execute calls) ---
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
        [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorSetLayout>
        create_descriptor_set_layout(const crd::rhi::DescriptorSetLayoutDesc&) override { return nullptr; }
        [[nodiscard]] std::unique_ptr<crd::rhi::PipelineLayout>
        create_pipeline_layout(const crd::rhi::PipelineLayoutDesc&) override { return nullptr; }
        [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorAllocator>
        create_descriptor_allocator(const crd::rhi::DescriptorAllocatorDesc&) override { return nullptr; }
        [[nodiscard]] crd::rhi::Queue& graphics_queue() noexcept override { return m_queue; }
        void wait_idle() override {}

        struct FakeQueue final : crd::rhi::Queue
        {
            bool submit(crd::rhi::CommandBuffer&, crd::rhi::Swapchain&) override { return true; }
            void present(crd::rhi::Swapchain&) override {}
            void wait_idle() override {}
        } m_queue;
    } fake_device;

    // --- Shader setup ---
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

    // --- Renderer v1d smoke: DrawList bucketing ---
    FakeBuffer vertex_buffer(
        {sizeof(float) * 15u, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});

    crd::renderer::Renderer renderer;

    auto make_renderable = [&](crd::renderer::DrawBucket bucket) -> crd::renderer::Renderable
    {
        crd::renderer::Renderable r;
        r.vertex_buffer = &vertex_buffer;
        r.vertex_count  = 3;
        r.variant       = variant;
        r.bucket        = bucket;
        return r;
    };

    // 2 opaque, 1 masked, 1 translucent
    renderer.submit(make_renderable(crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(crd::renderer::DrawBucket::Opaque));
    renderer.submit(make_renderable(crd::renderer::DrawBucket::Masked));
    renderer.submit(make_renderable(crd::renderer::DrawBucket::Translucent));

    crd::renderer::FrameContext ctx;
    ctx.viewport = {1280, 720};

    crd::renderer::DrawList draw_list;
    const bool built = renderer.build_frame(ctx, *runtime, draw_list);

    CRD_LOG_INFO(g_log_smoke_renderer, "build_frame={} opaque={} masked={} translucent={} total={}",
                 built, draw_list.opaque.size(), draw_list.masked.size(), draw_list.translucent.size(),
                 draw_list.total_count());

    const bool counts_ok = built &&
                           draw_list.opaque.size() == 2 &&
                           draw_list.masked.size() == 1 &&
                           draw_list.translucent.size() == 1;

    // --- IRenderPath smoke ---
    FakeImage          color_img({});
    FakeCommandBuffer  render_cmd;
    SimpleRenderPath   render_path(color_img);

    render_path.resize({1280, 720});

    crd::renderer::FrameGraph fg;
    render_path.build(fg, draw_list, ctx);

    const bool fg_render_built = fg.build();
    fg.execute(fake_device, render_cmd);
    // One transition: Undefined → ColorWrite for the imported color image.
    const bool fg_render_ok = fg_render_built && render_cmd.transition_count == 1;

    const bool path_ok = render_path.m_build_count      == 1 &&
                         render_path.m_last_opaque       == 2 &&
                         render_path.m_last_masked       == 1 &&
                         render_path.m_last_translucent  == 1 &&
                         render_path.m_resize_count      == 1 &&
                         render_path.output_image().is_valid() &&
                         fg_render_ok;

    CRD_LOG_INFO(g_log_smoke_renderer, "IRenderPath ok={} (build_count={} resize_count={} fg_ok={})",
                 path_ok, render_path.m_build_count, render_path.m_resize_count, fg_render_ok);

    // --- Frame graph multi-pass smoke (depth-prepass → main-color) ---
    // Demonstrates: import, multi-pass declaration, build, execute with barrier insertion.
    FakeCommandBuffer  fg_cmd;
    crd::renderer::FrameGraph frame_graph;

    FakeImage depth_img({});
    FakeImage color2_img({});
    auto depth_handle  = frame_graph.import(&depth_img,  crd::rhi::ImageAccess::Undefined);
    auto color2_handle = frame_graph.import(&color2_img, crd::rhi::ImageAccess::Undefined);

    {
        auto builder = frame_graph.add_pass("depth-prepass");
        builder.write(depth_handle, crd::rhi::ImageAccess::DepthWrite);
        builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    }
    {
        auto builder = frame_graph.add_pass("main-color");
        builder.read(depth_handle,  crd::rhi::ImageAccess::DepthRead);
        builder.write(color2_handle, crd::rhi::ImageAccess::ColorWrite);
        builder.set_execute([](crd::renderer::FrameResources&, crd::rhi::CommandBuffer&) {});
    }

    const bool multi_fg_built = frame_graph.build();
    CRD_LOG_INFO(g_log_smoke_renderer, "frame_graph.build()={}", multi_fg_built);

    frame_graph.execute(fake_device, fg_cmd);
    // Expect 3 barriers: Undef→DepthWrite, Undef→ColorWrite, DepthWrite→DepthRead
    CRD_LOG_INFO(g_log_smoke_renderer, "frame_graph.execute() transitions={}", fg_cmd.transition_count);
    const bool multi_fg_ok = multi_fg_built && fg_cmd.transition_count == 3;

    // --- Swapchain blit smoke (v1i) ---
    // Simulates the full end-of-frame sequence:
    //   render_output (ColorWrite) → swapchain-blit → present-barrier
    // Expected barriers: ColorWrite→TransferSrc, Undef→TransferDst, TransferDst→Present
    // Expected blit call: 1
    FakeCommandBuffer  blit_cmd;
    crd::renderer::FrameGraph blit_fg;

    FakeImage render_output_img({});
    FakeImage swapchain_img({});

    auto render_out_h = blit_fg.import(&render_output_img, crd::rhi::ImageAccess::ColorWrite);
    [[maybe_unused]] auto blit_sc_h = crd::renderer::add_swapchain_blit_pass(
        blit_fg, render_out_h, swapchain_img, {1280, 720}, {1280, 720});

    const bool blit_built = blit_fg.build();
    blit_fg.execute(fake_device, blit_cmd);

    const bool blit_ok = blit_built &&
                         blit_cmd.blit_count       == 1 &&
                         blit_cmd.transition_count == 3;

    CRD_LOG_INFO(g_log_smoke_renderer,
                 "swapchain-blit ok={} (blit={} transitions={})",
                 blit_ok, blit_cmd.blit_count, blit_cmd.transition_count);

    crd::log::flush();
    crd::log::shutdown();
    return (counts_ok && path_ok && multi_fg_ok && blit_ok) ? 0 : 1;
}
