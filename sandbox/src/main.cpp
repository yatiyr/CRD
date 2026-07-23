// sandbox/main.cpp — RET-5 pt 2 (ADR-0105): THE SANDBOX FLIP. The live Cerid window runs END TO END on the ONE
// graphics layer: a windowed VulkanGpuContext + IRasterContext + the RET-2 present surface (canvas blit + overlay
// composition) + the RET-5 ImGuiGpuBackend (render) + ImGui_ImplGlfw (platform input) + the perf-ui ProfilerPanel.
// No crd-rhi, no crd-renderer, no rhi swapchain, no rhi ImGuiLayer — the retiring stack is OUT of the sandbox.
//
// The old SandboxLayer/geometry/curves showcases (built on the frozen ForwardRenderPath) left the build with this
// flip; their content returns through the RET-6 draw port onto gpu-context. The canvas scene today is a CKIR-drawn
// triangle — the point of THIS slice is the shell: window → context → canvas → blit → ImGui overlay → present.
//
// CLI contract (the full-sweep smoke depends on it — preserved exactly):
//   --headless                    — exit after 1 frame; minimal boot smoke
//   --smoke-test [duration_secs]  — run the loop N seconds (default 3.0), FAIL (exit 2) if nothing presented.

#include <crd/app/app.hpp>
#include <crd/draw/overlay_pass.hpp> // RET-6 pt 4: the debug-draw scene through gpu-context (grid + shapes)
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/gpu/context.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/imgui/imgui_gpu_backend.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/ui/ui.hpp>

#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_sandbox, "Sandbox", crd::log::LogLevel::Trace)

namespace
{

// The canvas scene: a CKIR clip-space triangle (VertexIndex → position, warm constant color). Deliberately minimal —
// the shell is the slice; the real showcases return via the RET-6 draw port.
void build_sandbox_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir  = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int  k0  = g.constant(0.0, sh, kir::DType::I32);
    const int  k1  = g.constant(1.0, sh, kir::DType::I32);
    const int  eq0 = g.binary(kir::KOp::CmpEq, vid, k0);
    const int  eq1 = g.binary(kir::KOp::CmpEq, vid, k1);
    const int  a   = g.constant(-0.8, sh, kir::DType::F32);
    const int  b   = g.constant(0.8, sh, kir::DType::F32);
    const int  c   = g.constant(0.0, sh, kir::DType::F32);
    const int  x   = g.select(eq0, a, g.select(eq1, b, c));
    const int  y   = g.select(eq0, b, g.select(eq1, b, a));
    ve.stage    = crd::kir::KStage::Vertex;
    ve.position = g.vec4(x, y, c, g.constant(1.0, sh, kir::DType::F32));
    ve.n_out    = 0;
}

void build_sandbox_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});
    const int  r  = g.constant(0.93, sh, kir::DType::F32);
    const int  gr = g.constant(0.42, sh, kir::DType::F32);
    const int  b  = g.constant(0.18, sh, kir::DType::F32);
    fe.stage      = kir::KStage::Fragment;
    fe.n_out      = 1;
    fe.out[0]     = {g.vec4(r, gr, b, g.constant(1.0, sh, kir::DType::F32)), 0};
}

[[nodiscard]] void* native_window_of(crd::app::Application& app)
{
#ifdef _WIN32
    return glfwGetWin32Window(static_cast<GLFWwindow*>(app.window().native_handle()));
#else
    (void)app;
    return nullptr; // the Linux platform surface rides the RET-8 cross-platform sweep
#endif
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered so log lines survive a crash

    bool     headless         = false;
    bool     smoke_test       = false;
    crd::f64 smoke_duration_s = 3.0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--headless") == 0) { headless = true; }
        else if (std::strcmp(argv[i], "--smoke-test") == 0)
        {
            smoke_test = true;
            if (i + 1 < argc)
            {
                char*          end = nullptr;
                const crd::f64 v   = std::strtod(argv[i + 1], &end);
                if (end != argv[i + 1] && v > 0.0)
                {
                    smoke_duration_s = v;
                    ++i;
                }
            }
        }
    }

    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    crd::app::ApplicationDesc app_desc;
    app_desc.window.title = crd::containers::String("Cerid Sandbox — gpu-context (ADR-0105)");
    app_desc.window.size  = {1280, 720};

    crd::app::Application app(app_desc);
    if (!app.is_valid())
    {
        CRD_LOG_ERROR(g_log_sandbox, "Application init failed");
        crd::log::shutdown();
        return 1;
    }

    // THE ONE GRAPHICS LAYER: windowed context → raster context → present surface. Nothing rhi anywhere.
    crd::gpu::GpuContextConfig gpu_cfg;
    gpu_cfg.backend           = crd::gpu::GpuBackend::Vulkan;
    gpu_cfg.headless          = false;
    gpu_cfg.enable_validation = !headless; // dev + smoke runs validated (the sweep smoke exists to catch validation bugs)
    auto gpu_context          = crd::gpu::create_vulkan_gpu_context(gpu_cfg);
    auto* vk = gpu_context != nullptr ? static_cast<crd::gpu::VulkanGpuContext*>(gpu_context.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        CRD_LOG_ERROR(g_log_sandbox, "GPU bootstrap failed — no graphics-capable Vulkan device / shader objects");
        crd::log::shutdown();
        return 1;
    }
    auto raster = crd::gpu::create_vulkan_raster_context(*vk);
    if (raster == nullptr || !vk->present_capable())
    {
        CRD_LOG_ERROR(g_log_sandbox, "Raster context / present capability unavailable");
        crd::log::shutdown();
        return 1;
    }
    const auto fb    = app.window().framebuffer_size();
    crd::u32   win_w = fb.width > 0 ? static_cast<crd::u32>(fb.width) : 1280U;
    crd::u32   win_h = fb.height > 0 ? static_cast<crd::u32>(fb.height) : 720U;
    auto     surface = raster->create_present_surface(native_window_of(app), win_w, win_h, crd::gpu::PresentMode::Fifo);
    if (surface == nullptr)
    {
        CRD_LOG_ERROR(g_log_sandbox, "Present surface creation failed");
        crd::log::shutdown();
        return 1;
    }

    // the CKIR canvas scene
    crd::memory::TlsfAllocator kir_alloc(8U << 20U);
    crd::kir::KGraph           vg(&kir_alloc);
    crd::kir::KEntry           ve;
    build_sandbox_vs(vg, ve);
    crd::kir::KGraph fg(&kir_alloc);
    crd::kir::KEntry fe;
    build_sandbox_fs(fg, fe);
    auto vs      = vk->create_program(vg, ve);
    auto fs      = vk->create_program(fg, fe);
    auto program = (vs != nullptr && fs != nullptr) ? raster->create_raster_program(*vs, *fs) : nullptr;
    auto canvas  = raster->create_color_target(surface->width(), surface->height());
    if (program == nullptr || canvas == nullptr)
    {
        CRD_LOG_ERROR(g_log_sandbox, "CKIR canvas bootstrap failed");
        crd::log::shutdown();
        return 1;
    }

    // ImGui: the GLFW platform half + the gpu-context render half (RET-5), composited at the present seam
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(app.window().native_handle()), true);
    auto imgui_backend = std::make_unique<crd::imgui::ImGuiGpuBackend>(*vk, *surface);
    if (!imgui_backend->valid())
    {
        CRD_LOG_ERROR(g_log_sandbox, "ImGui gpu backend init failed");
        crd::log::shutdown();
        return 1;
    }

    // RET-6 pt 4: the debug-draw scene on the ONE layer — the CKIR draw suite compiled at init, the demo content a
    // RenderBuffer of shapes composed over the canvas by submit_overlay each frame (the editor-grid look returns).
    const bool draw_ready = crd::draw::init(*vk, *raster);
    if (!draw_ready) { CRD_LOG_WARN(g_log_sandbox, "crd-draw init failed -- continuing without the draw overlay"); }
    crd::draw::RenderBuffer draw_buf(crd::memory::default_allocator());
    if (draw_ready)
    {
        crd::draw::axis_triad_to(draw_buf, crd::math::Mat4f::identity(), 1.5F, 3.0F);
        crd::draw::sphere_wire_to(draw_buf, {2.5F, 1.0F, 0.0F}, 1.0F, crd::draw::kCyan);
        crd::math::Mat4f box_world = crd::math::Mat4f::identity();
        box_world.c3               = {-2.5F, 0.75F, 0.5F, 1.0F};
        crd::draw::box_wire_to(draw_buf, box_world, {0.75F, 0.75F, 0.75F}, crd::draw::kOrange, 2.0F);
        crd::draw::capsule_wire_to(draw_buf, {0.0F, 0.5F, -2.5F}, {0.0F, 2.0F, -2.5F}, 0.5F, crd::draw::kMagenta);
        crd::math::Mat4f slab_world = crd::math::Mat4f::identity();
        slab_world.c3               = {0.0F, 0.25F, 2.5F, 1.0F};
        crd::draw::box_solid_to(draw_buf, slab_world, {1.0F, 0.25F, 0.6F},
                                crd::draw::Color{255, 200, 40, 90}); // translucent amber slab (alpha blending live)
    }

    // perf substrate + the ProfilerPanel overlay. (The rhi GPU-profiler backend died with the flip; the gpu-context
    // profiler backend arrives with the RET-7 sweep — CPU spans + allocator tracking are live today.)
    crd::perf::init({});
    [[maybe_unused]] const auto perf_alloc_idx =
        crd::perf::register_allocator("default (malloc)", crd::memory::default_allocator());
    crd::perf::install_jobs_adapter();
    crd::perf::ui::ProfilerPanel profiler_panel;

    crd::jobs::init(app_desc.jobs_config);
    CRD_LOG_INFO(g_log_sandbox, "Sandbox on gpu-context (headless={} smoke_test={} duration={}s)", headless, smoke_test,
                 smoke_duration_s);

    crd::u32   frame               = 0;
    crd::u32   frames_with_present = 0;
    const auto smoke_start_time    = std::chrono::steady_clock::now();
    while (app.is_running())
    {
        if (!app.tick()) { break; }

        // window resize → recreate the swapchain + a matching canvas
        const auto     cur   = app.window().framebuffer_size();
        const crd::u32 cur_w = cur.width > 0 ? static_cast<crd::u32>(cur.width) : 0U;
        const crd::u32 cur_h = cur.height > 0 ? static_cast<crd::u32>(cur.height) : 0U;
        if ((cur_w != win_w || cur_h != win_h) && cur_w > 0U && cur_h > 0U)
        {
            win_w = cur_w;
            win_h = cur_h;
            if (surface->resize(win_w, win_h))
            {
                canvas = raster->create_color_target(surface->width(), surface->height());
                if (canvas == nullptr) { break; }
            }
        }

        raster->draw(*canvas, *program, crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F}, 3U);

        // the debug-draw scene: a slow orbit camera + the infinite grid + the shape set, composed over the canvas
        if (draw_ready)
        {
            const crd::f64 tsec =
                std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - smoke_start_time).count();
            const float            orbit = static_cast<float>(tsec * 0.25);
            const crd::math::Vec3f eye{crd::math::sin(orbit) * 7.0F, 3.5F, crd::math::cos(orbit) * 7.0F};
            const crd::math::Mat4f view = crd::math::look_at(eye, crd::math::Vec3f{0.0F, 0.75F, 0.0F},
                                                             crd::math::Vec3f{0.0F, 1.0F, 0.0F});
            const float            aspect =
                static_cast<float>(surface->width()) / static_cast<float>(surface->height() > 0U ? surface->height() : 1U);
            const crd::math::Mat4f proj = crd::math::perspective_reverse_z(1.0472F, aspect, 0.1F); // 60 deg fov

            crd::draw::OverlayPassConfig ocfg;
            ocfg.view_proj   = proj * view;
            ocfg.viewport_px = {static_cast<crd::f32>(surface->width()), static_cast<crd::f32>(surface->height())};
            ocfg.time_s      = static_cast<crd::f32>(tsec);
            ocfg.depth_test  = crd::gpu::DepthCompare::GreaterEqual; // reverse-Z (ignored on the depthless canvas today)
            ocfg.grid.enabled    = true;
            ocfg.grid.camera_pos = eye;
            ocfg.grid.apply_theme();
            if (!crd::draw::submit_overlay(*canvas, draw_buf, ocfg))
            {
                CRD_LOG_WARN(g_log_sandbox, "draw overlay submission refused");
            }
        }

        imgui_backend->new_frame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        {
            ImGui::Begin("Cerid Sandbox");
            ImGui::Text("gpu-context end to end (ADR-0105)");
            ImGui::Text("frame %u  |  %ux%u", frame, surface->width(), surface->height());
            ImGui::TextUnformatted("scene: CKIR canvas -> draw overlay (grid+shapes) -> blit -> ImGui -> present");
            ImGui::End();
            profiler_panel.draw();
        }
        ImGui::Render();

        if (surface->present(*canvas, &crd::imgui::ImGuiGpuBackend::overlay_thunk, imgui_backend.get()))
        {
            ++frames_with_present;
        }

        crd::perf::frame_mark();
        ++frame;

        if (headless && frame >= 1)
        {
            CRD_LOG_INFO(g_log_sandbox, "Headless: exiting after {} frame(s)", frame);
            app.close();
        }
        if (smoke_test)
        {
            const auto     now     = std::chrono::steady_clock::now();
            const crd::f64 elapsed = std::chrono::duration<crd::f64>(now - smoke_start_time).count();
            if (elapsed >= smoke_duration_s)
            {
                if (frames_with_present == 0)
                {
                    CRD_LOG_ERROR(g_log_sandbox, "Smoke-test: 0 frames presented in {:.2f}s — failure", elapsed);
                    crd::log::flush();
                    crd::log::shutdown();
                    return 2;
                }
                CRD_LOG_INFO(g_log_sandbox, "Smoke-test: PASS — {} frames presented over {:.2f}s ({:.1f} fps avg)",
                             frames_with_present, elapsed,
                             static_cast<crd::f64>(frames_with_present) / elapsed);
                app.close();
            }
        }
        if (app.window().input().state().was_key_pressed(crd::platform::Key::Escape)) { app.close(); }
    }

    // Teardown order: ImGui backend (drains the device) → GLFW platform half → context → present surface →
    // canvas/program (before the raster context) → raster → gpu context. jobs/perf mirror their bring-up.
    crd::jobs::shutdown();
    crd::perf::uninstall_jobs_adapter();
    crd::perf::shutdown();
    crd::draw::shutdown(); // the draw renderer's GPU objects die before the raster context
    imgui_backend.reset();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    surface.reset();
    canvas.reset();
    program.reset();
    vs.reset();
    fs.reset();
    raster.reset();
    gpu_context.reset();

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
