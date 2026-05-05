#include "sandbox_layer.hpp"

#include <crd/app/event_dispatcher.hpp>
#include <crd/app/events/input_events.hpp>
#include <crd/app/events/window_events.hpp>
#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/meshgen/meshgen.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/input.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/per_frame_data.hpp>
#include <crd/shader/effect.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <numbers>

CRD_DEFINE_LOG_CHANNEL(g_log_sandbox_layer, "SandboxLayer", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

namespace crd::sandbox
{
namespace
{
constexpr float kOrbitSpeed  = 8.0F;
constexpr float kOrbitSens   = 0.1F;   // radians per mouse delta unit
constexpr float kPanSpeed    = 0.005F;
constexpr float kZoomSpeed   = 0.5F;
constexpr float kMinDistance = 0.1F;
constexpr float kMaxDistance = 500.0F;

float exp_lerp(float a, float b, float speed, float dt) noexcept
{
    return a + (b - a) * (1.0F - std::exp(-speed * dt));
}

crd::math::Vec3f exp_lerp3(crd::math::Vec3f a, crd::math::Vec3f b, float speed, float dt) noexcept
{
    const float t = 1.0F - std::exp(-speed * dt);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}


} // namespace

// ─── SandboxPipelineResolver ─────────────────────────────────────────────────

void SandboxPipelineResolver::init(crd::rhi::Device& device, crd::rhi::PipelineLayout& layout,
                                   crd::shader::Runtime& runtime, crd::shader::VariantHandle variant,
                                   crd::rhi::Extent2D extent)
{
    m_device  = &device;
    m_layout  = &layout;
    m_runtime = &runtime;
    m_variant = variant;
    m_extent  = extent;
}

void SandboxPipelineResolver::begin_pass(crd::renderer::PassType pass) noexcept
{
    m_current_pass = pass;
}

[[nodiscard]] crd::rhi::Pipeline*
SandboxPipelineResolver::resolve_pipeline(const crd::shader::VariantPipelineDesc& handoff) noexcept
{
    if (!m_compiled)
        ensure_compiled(handoff);

    if (m_current_pass == crd::renderer::PassType::DepthPrepass)
        return m_depth_pipeline.get();
    return m_color_pipeline.get();
}

void SandboxPipelineResolver::ensure_compiled(const crd::shader::VariantPipelineDesc& handoff) noexcept
{
    m_compiled = true; // mark even if compilation fails, to avoid retry on every draw

    if (handoff.modules.empty() || m_device == nullptr || m_layout == nullptr || m_runtime == nullptr)
        return;

    // Standard 48-byte interleaved vertex layout.
    const crd::rhi::VertexBindingDesc binding{0, 48, crd::rhi::VertexInputRate::Vertex};
    const crd::rhi::VertexAttributeDesc attrs[4] = {
        {0, 0, crd::rhi::Format::R32G32B32Sfloat,    0},   // position
        {1, 0, crd::rhi::Format::R32G32B32Sfloat,    12},  // normal
        {2, 0, crd::rhi::Format::R32G32Sfloat,       24},  // uv0
        {3, 0, crd::rhi::Format::R32G32B32A32Sfloat, 32},  // tangent
    };

    const crd::shader::Module* vert_module = nullptr;
    const crd::shader::Module* frag_module = nullptr;

    for (const auto& mod_usage : handoff.modules)
    {
        const crd::shader::Module* mod = m_runtime->find_module(mod_usage.module);
        if (mod == nullptr)
            continue;
        if (mod->stage() == crd::shader::Stage::Vertex)
            vert_module = mod;
        else if (mod->stage() == crd::shader::Stage::Fragment)
            frag_module = mod;
    }

    if (vert_module == nullptr)
        return;

    const auto vert_bytes = vert_module->code_bytes();

    // Depth-only pipeline: vertex shader only, Undefined color format.
    {
        auto vert_mod = m_device->create_shader_module(
            {crd::rhi::ShaderStage::Vertex, "main",
             crd::containers::make_span(vert_bytes.data(), vert_bytes.size())});
        if (vert_mod)
        {
            crd::rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader        = vert_mod.get();
            desc.fragment_shader      = nullptr;
            desc.topology             = crd::rhi::PrimitiveTopology::TriangleList;
            desc.viewport_extent      = m_extent;
            desc.color_format         = crd::rhi::Format::Undefined;
            desc.depth_format         = crd::rhi::Format::D32Sfloat;
            desc.vertex_bindings      = crd::containers::make_span(&binding, 1U);
            desc.vertex_attributes    = crd::containers::make_span(attrs, 4U);
            desc.enable_depth_test    = true;
            desc.enable_blend         = false;
            desc.use_dynamic_viewport = true;
            desc.pipeline_layout      = m_layout;
            m_depth_pipeline = m_device->create_graphics_pipeline(desc);
        }
    }

    if (frag_module == nullptr)
        return;

    const auto frag_bytes = frag_module->code_bytes();

    // Color pipeline: vert + frag, B8G8R8A8Unorm color format.
    {
        auto vert_mod = m_device->create_shader_module(
            {crd::rhi::ShaderStage::Vertex, "main",
             crd::containers::make_span(vert_bytes.data(), vert_bytes.size())});
        auto frag_mod = m_device->create_shader_module(
            {crd::rhi::ShaderStage::Fragment, "main",
             crd::containers::make_span(frag_bytes.data(), frag_bytes.size())});
        if (vert_mod && frag_mod)
        {
            crd::rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader        = vert_mod.get();
            desc.fragment_shader      = frag_mod.get();
            desc.topology             = crd::rhi::PrimitiveTopology::TriangleList;
            desc.viewport_extent      = m_extent;
            desc.color_format         = crd::rhi::Format::B8G8R8A8Unorm;
            desc.depth_format         = crd::rhi::Format::D32Sfloat;
            desc.vertex_bindings      = crd::containers::make_span(&binding, 1U);
            desc.vertex_attributes    = crd::containers::make_span(attrs, 4U);
            desc.enable_depth_test    = true;
            desc.enable_blend         = false;
            desc.use_dynamic_viewport = true;
            desc.pipeline_layout      = m_layout;
            m_color_pipeline = m_device->create_graphics_pipeline(desc);
        }
    }
}

// ─── SandboxLayer ────────────────────────────────────────────────────────────

SandboxLayer::SandboxLayer(crd::app::Application& app, crd::rhi::Device& device,
                           crd::rhi::Swapchain& swapchain)
    : Layer("SandboxLayer"), m_app(app), m_device(device), m_swapchain(swapchain),
      m_shapes(&m_alloc), m_gpu_mesh(nullptr, nullptr)
{
    // Populate shape list (metadata only — no GPU upload yet).
    crd::memory::MallocAllocator tmp;
    auto record = [&](const char* nm, crd::renderer::MeshResource mesh)
    {
        m_shapes.push_back(ShapeInfo{nm, mesh.primitives[0].vertex_count, mesh.primitives[0].index_count});
    };
    record("Plane",     crd::meshgen::make_plane(&tmp));
    record("Box",       crd::meshgen::make_box(&tmp));
    record("Sphere",    crd::meshgen::make_sphere(&tmp));
    record("Icosphere", crd::meshgen::make_icosphere(&tmp));
    record("Cylinder",  crd::meshgen::make_cylinder(&tmp));
    record("Cone",      crd::meshgen::make_cone(&tmp));
    record("Capsule",   crd::meshgen::make_capsule(&tmp));
    record("Torus",     crd::meshgen::make_torus(&tmp));

    // Compile surface shaders via Runtime.
    m_shader_runtime = crd::shader::create_runtime();
    CRD_ASSERT(m_shader_runtime != nullptr);

    const fs::Path source_dir(CRD_SOURCE_DIR);
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("surface");
    desc.frontend_modules.push_back(
        {crd::containers::String((source_dir / "assets/shaders/surface.vert").generic().data()),
         crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back(
        {crd::containers::String((source_dir / "assets/shaders/surface.frag").generic().data()),
         crd::shader::Stage::Fragment, crd::containers::String("main")});

    const auto effect = m_shader_runtime->create_effect(desc);
    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest req;
    req.effect = effect;
    m_surface_variant = m_shader_runtime->request_variant(req, diagnostics);
    if (!m_surface_variant.is_valid())
    {
        CRD_LOG_ERROR(g_log_sandbox_layer, "surface shader compilation failed: {}",
                      diagnostics.message.c_str());
    }

    // Descriptor allocator for ForwardRenderPath.
    m_desc_alloc = device.create_descriptor_allocator({2, 512});
    CRD_ASSERT(m_desc_alloc != nullptr);

    const crd::rhi::Extent2D extent = swapchain.desc().extent;

    // Create ForwardRenderPath — resolver is init'd after we have the pipeline layout.
    m_frp = crd::renderer::ForwardRenderPath::create(device, m_resolver, *m_desc_alloc, extent, 2);
    CRD_ASSERT(m_frp != nullptr);

    m_resolver.init(device, m_frp->pipeline_layout(), *m_shader_runtime, m_surface_variant, extent);

    // Build wireframe overlay pipeline.
    build_wireframe_pipeline(source_dir);
}

void SandboxLayer::build_wireframe_pipeline(const crd::platform::fs::Path& source_dir)
{
    // Standalone layout: no descriptor sets, 64-byte MVP push constant (Vertex stage only).
    const crd::rhi::PushConstantRange wf_push{crd::rhi::ShaderStage::Vertex, 0, 64U};
    m_wf_layout = m_device.create_pipeline_layout({
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>{},
        crd::containers::make_span(&wf_push, 1U),
    });
    if (!m_wf_layout)
    {
        CRD_LOG_ERROR(g_log_sandbox_layer, "wireframe: failed to create pipeline layout");
        return;
    }

    crd::shader::EffectDesc wf_eff;
    wf_eff.name = crd::containers::String("wireframe");
    wf_eff.frontend_modules.push_back(
        {crd::containers::String((source_dir / "assets/shaders/wireframe.vert").generic().data()),
         crd::shader::Stage::Vertex, crd::containers::String("main")});
    wf_eff.frontend_modules.push_back(
        {crd::containers::String((source_dir / "assets/shaders/wireframe.frag").generic().data()),
         crd::shader::Stage::Fragment, crd::containers::String("main")});

    const auto wf_effect = m_shader_runtime->create_effect(wf_eff);
    crd::shader::CompileDiagnostics wf_diag;
    crd::shader::VariantCompileRequest wf_req;
    wf_req.effect = wf_effect;
    const auto wf_variant = m_shader_runtime->request_variant(wf_req, wf_diag);
    if (!wf_variant.is_valid())
    {
        CRD_LOG_ERROR(g_log_sandbox_layer, "wireframe shader compilation failed: {}",
                      wf_diag.message.c_str());
        return;
    }

    const crd::shader::Module* vert_mod = nullptr;
    const crd::shader::Module* frag_mod = nullptr;
    for (const auto mh : m_shader_runtime->variant_modules(wf_variant))
    {
        const crd::shader::Module* mod = m_shader_runtime->find_module(mh);
        if (mod == nullptr)
            continue;
        if (mod->stage() == crd::shader::Stage::Vertex)
            vert_mod = mod;
        else if (mod->stage() == crd::shader::Stage::Fragment)
            frag_mod = mod;
    }
    if (vert_mod == nullptr || frag_mod == nullptr)
    {
        CRD_LOG_ERROR(g_log_sandbox_layer, "wireframe: missing vert or frag module after compile");
        return;
    }

    const auto vert_bytes = vert_mod->code_bytes();
    const auto frag_bytes = frag_mod->code_bytes();

    auto vk_vert = m_device.create_shader_module(
        {crd::rhi::ShaderStage::Vertex, "main",
         crd::containers::make_span(vert_bytes.data(), vert_bytes.size())});
    auto vk_frag = m_device.create_shader_module(
        {crd::rhi::ShaderStage::Fragment, "main",
         crd::containers::make_span(frag_bytes.data(), frag_bytes.size())});
    if (!vk_vert || !vk_frag)
    {
        CRD_LOG_ERROR(g_log_sandbox_layer, "wireframe: failed to create shader modules");
        return;
    }

    // Position-only vertex attribute; stride still 48 to match the shared vertex buffer layout.
    const crd::rhi::VertexBindingDesc   wf_binding{0, 48, crd::rhi::VertexInputRate::Vertex};
    const crd::rhi::VertexAttributeDesc wf_attr{0, 0, crd::rhi::Format::R32G32B32Sfloat, 0};

    crd::rhi::GraphicsPipelineDesc wf_desc;
    wf_desc.vertex_shader        = vk_vert.get();
    wf_desc.fragment_shader      = vk_frag.get();
    wf_desc.topology             = crd::rhi::PrimitiveTopology::TriangleList;
    wf_desc.viewport_extent      = m_swapchain.desc().extent;
    wf_desc.color_format         = crd::rhi::Format::B8G8R8A8Unorm;
    wf_desc.depth_format         = crd::rhi::Format::Undefined;
    wf_desc.vertex_bindings      = crd::containers::make_span(&wf_binding, 1U);
    wf_desc.vertex_attributes    = crd::containers::make_span(&wf_attr, 1U);
    wf_desc.enable_depth_test    = false;
    wf_desc.enable_blend         = false;
    wf_desc.use_dynamic_viewport = true;
    wf_desc.wireframe            = true;
    wf_desc.depth_write          = false;
    wf_desc.pipeline_layout      = m_wf_layout.get();

    m_wf_pipeline = m_device.create_graphics_pipeline(wf_desc);
    if (!m_wf_pipeline)
        CRD_LOG_ERROR(g_log_sandbox_layer, "wireframe: failed to create graphics pipeline");
}

void SandboxLayer::on_update(crd::f64 delta_seconds)
{
    const auto& input = m_app.window().input().state();
    const float dt    = static_cast<float>(delta_seconds);

    const bool imgui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

    if (!imgui_wants_mouse)
    {
        if (input.is_mouse_down(crd::platform::MouseButton::Left))
        {
            const auto q_yaw   = crd::math::from_axis_angle(crd::math::Vec3f{0.0F, 1.0F, 0.0F},
                                                              input.mouse_dx() * kOrbitSens);
            const auto q_pitch = crd::math::from_axis_angle(crd::math::Vec3f{1.0F, 0.0F, 0.0F},
                                                              -input.mouse_dy() * kOrbitSens);
            m_cam.q_target = crd::math::normalized(q_yaw * m_cam.q_target * q_pitch);
        }

        if (input.is_mouse_down(crd::platform::MouseButton::Middle) &&
            (input.is_key_down(crd::platform::Key::LeftCtrl) || input.is_key_down(crd::platform::Key::RightCtrl)))
        {
            const auto  cam_right = crd::math::rotate_vector(m_cam.q_target, crd::math::Vec3f{1.0F, 0.0F, 0.0F});
            const auto  cam_up    = crd::math::rotate_vector(m_cam.q_target, crd::math::Vec3f{0.0F, 1.0F, 0.0F});
            const float scale     = m_cam.distance * kPanSpeed;
            m_cam.target -= cam_right * (input.mouse_dx() * scale);
            m_cam.target += cam_up    * (input.mouse_dy() * scale);
        }

        const float scroll = input.scroll_dy();
        if (scroll != 0.0F)
        {
            m_cam.distance -= scroll * kZoomSpeed * (m_cam.distance * 0.2F + 0.1F);
            m_cam.distance  = std::clamp(m_cam.distance, kMinDistance, kMaxDistance);
        }
    }

    const float smooth_t = 1.0F - std::exp(-kOrbitSpeed * dt);
    m_cam.q_smooth = crd::math::slerp(m_cam.q_smooth, m_cam.q_target, smooth_t);
    m_cam.s_dist   = exp_lerp(m_cam.s_dist,  m_cam.distance, kOrbitSpeed, dt);
    m_cam.s_target = exp_lerp3(m_cam.s_target, m_cam.target, kOrbitSpeed, dt);

    // Upload mesh when selection changes or parameters are edited.
    const bool shape_changed  = m_selected != m_last_uploaded;
    const bool params_changed = m_mesh_dirty;
    if ((shape_changed || params_changed) && m_selected >= 0 && m_selected < static_cast<int>(m_shapes.size()))
    {
        upload_selected_shape();
        m_mesh_dirty = false;
    }
}

void SandboxLayer::upload_selected_shape()
{
    m_last_uploaded = m_selected;
    m_device.wait_idle(); // safe: previous frame complete before we free old buffers

    crd::memory::MallocAllocator tmp;
    crd::renderer::MeshResource cpu_mesh(&tmp);
    switch (m_selected)
    {
    case 0:
        cpu_mesh = crd::meshgen::make_plane(&tmp, m_plane.w, m_plane.d,
                                            static_cast<crd::u32>(m_plane.divs_x),
                                            static_cast<crd::u32>(m_plane.divs_z));
        break;
    case 1:
        cpu_mesh = crd::meshgen::make_box(&tmp, m_box.w, m_box.h, m_box.d);
        break;
    case 2:
        cpu_mesh = crd::meshgen::make_sphere(&tmp, m_sphere.radius,
                                             static_cast<crd::u32>(m_sphere.lat),
                                             static_cast<crd::u32>(m_sphere.lon));
        break;
    case 3:
        cpu_mesh = crd::meshgen::make_icosphere(&tmp, m_ico.radius,
                                                static_cast<crd::u32>(m_ico.subdiv));
        break;
    case 4:
        cpu_mesh = crd::meshgen::make_cylinder(&tmp, m_cylinder.radius, m_cylinder.h,
                                               static_cast<crd::u32>(m_cylinder.segs));
        break;
    case 5:
        cpu_mesh = crd::meshgen::make_cone(&tmp, m_cone.radius, m_cone.h,
                                           static_cast<crd::u32>(m_cone.segs));
        break;
    case 6:
        cpu_mesh = crd::meshgen::make_capsule(&tmp, m_capsule.radius, m_capsule.h,
                                              static_cast<crd::u32>(m_capsule.segs),
                                              static_cast<crd::u32>(m_capsule.rings));
        break;
    case 7:
        cpu_mesh = crd::meshgen::make_torus(&tmp, m_torus.maj_r, m_torus.min_r,
                                            static_cast<crd::u32>(m_torus.maj_segs),
                                            static_cast<crd::u32>(m_torus.min_segs));
        break;
    default: return;
    }

    // Update display counts for the new parameters.
    auto& si   = m_shapes[static_cast<crd::usize>(m_selected)];
    si.verts   = cpu_mesh.primitives[0].vertex_count;
    si.indices = cpu_mesh.primitives[0].index_count;

    m_gpu_mesh = crd::renderer::GpuUploader::upload_mesh(cpu_mesh, m_device);
    CRD_LOG_INFO(g_log_sandbox_layer, "Uploaded shape '{}': {} verts, {} indices",
                 si.name, si.verts, si.indices);
}

void SandboxLayer::on_render()
{
    const auto& ext = m_swapchain.desc().extent;
    ImGui::SetNextWindowPos({8.0F, 8.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({320.0F, 160.0F}, ImGuiCond_Always);
    ImGui::Begin("Sandbox", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Viewport: %u x %u", ext.width, ext.height);
    ImGui::Separator();
    ImGui::Text("Camera (smoothed)");
    ImGui::Text("  distance: %.2f", static_cast<double>(m_cam.s_dist));
    ImGui::Text("  target:   (%.2f, %.2f, %.2f)",
                static_cast<double>(m_cam.s_target.x),
                static_cast<double>(m_cam.s_target.y),
                static_cast<double>(m_cam.s_target.z));
    ImGui::Text("  orient:   (%.3f, %.3f, %.3f, %.3f)",
                static_cast<double>(m_cam.q_smooth.x),
                static_cast<double>(m_cam.q_smooth.y),
                static_cast<double>(m_cam.q_smooth.z),
                static_cast<double>(m_cam.q_smooth.w));
    ImGui::Separator();
    ImGui::TextDisabled("LMB drag=orbit  Ctrl+MMB=pan  Scroll=zoom");
    ImGui::End();

    ImGui::SetNextWindowPos({8.0F, 176.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({320.0F, 560.0F}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Meshgen Browser", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // Render mode.
    ImGui::Text("Render Mode");
    ImGui::Checkbox("Solid",     &m_show_solid);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &m_show_wireframe);
    ImGui::Separator();

    ImGui::Text("Procedural Shapes (%u)", static_cast<unsigned>(m_shapes.size()));
    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(m_shapes.size()); ++i)
    {
        if (ImGui::Selectable(m_shapes[i].name, m_selected == i))
            m_selected = i;
    }
    ImGui::Separator();
    if (m_selected >= 0 && m_selected < static_cast<int>(m_shapes.size()))
    {
        const auto& s = m_shapes[static_cast<crd::usize>(m_selected)];
        ImGui::Text("Name:    %s",  s.name);
        ImGui::Text("Verts:   %u",  s.verts);
        ImGui::Text("Indices: %u",  s.indices);
        ImGui::Text("Tris:    %u",  s.indices / 3U);

        ImGui::Separator();
        ImGui::Text("Parameters");

        auto dirty = [this]() { if (ImGui::IsItemDeactivatedAfterEdit()) m_mesh_dirty = true; };

        switch (m_selected)
        {
        case 0: // Plane
            ImGui::SliderFloat("Width##pl",  &m_plane.w, 0.1F, 10.0F); dirty();
            ImGui::SliderFloat("Depth##pl",  &m_plane.d, 0.1F, 10.0F); dirty();
            ImGui::SliderInt("Divs X##pl",   &m_plane.divs_x, 1, 32);  dirty();
            ImGui::SliderInt("Divs Z##pl",   &m_plane.divs_z, 1, 32);  dirty();
            break;
        case 1: // Box
            ImGui::SliderFloat("Width##bx",  &m_box.w, 0.1F, 5.0F); dirty();
            ImGui::SliderFloat("Height##bx", &m_box.h, 0.1F, 5.0F); dirty();
            ImGui::SliderFloat("Depth##bx",  &m_box.d, 0.1F, 5.0F); dirty();
            break;
        case 2: // Sphere
            ImGui::SliderFloat("Radius##sp", &m_sphere.radius, 0.1F, 5.0F); dirty();
            ImGui::SliderInt("Lat bands##sp", &m_sphere.lat,   4, 64);      dirty();
            ImGui::SliderInt("Lon bands##sp", &m_sphere.lon,   4, 64);      dirty();
            break;
        case 3: // Icosphere
            ImGui::SliderFloat("Radius##ic",       &m_ico.radius, 0.1F, 5.0F); dirty();
            ImGui::SliderInt("Subdivisions##ic",   &m_ico.subdiv, 0, 5);       dirty();
            break;
        case 4: // Cylinder
            ImGui::SliderFloat("Radius##cy",  &m_cylinder.radius, 0.05F, 3.0F); dirty();
            ImGui::SliderFloat("Height##cy",  &m_cylinder.h,      0.1F,  5.0F); dirty();
            ImGui::SliderInt("Segments##cy",  &m_cylinder.segs,   3, 64);       dirty();
            break;
        case 5: // Cone
            ImGui::SliderFloat("Radius##co",  &m_cone.radius, 0.05F, 3.0F); dirty();
            ImGui::SliderFloat("Height##co",  &m_cone.h,      0.1F,  5.0F); dirty();
            ImGui::SliderInt("Segments##co",  &m_cone.segs,   3, 64);       dirty();
            break;
        case 6: // Capsule
            ImGui::SliderFloat("Radius##ca",  &m_capsule.radius, 0.05F, 3.0F); dirty();
            ImGui::SliderFloat("Height##ca",  &m_capsule.h,      0.1F,  5.0F); dirty();
            ImGui::SliderInt("Segments##ca",  &m_capsule.segs,   3, 64);       dirty();
            ImGui::SliderInt("Rings##ca",     &m_capsule.rings,  2, 16);       dirty();
            break;
        case 7: // Torus
            ImGui::SliderFloat("Major R##to",  &m_torus.maj_r,    0.2F, 5.0F); dirty();
            ImGui::SliderFloat("Minor R##to",  &m_torus.min_r,    0.05F, 2.0F); dirty();
            ImGui::SliderInt("Maj segs##to",   &m_torus.maj_segs, 4, 64);      dirty();
            ImGui::SliderInt("Min segs##to",   &m_torus.min_segs, 4, 32);      dirty();
            break;
        default: break;
        }
    }
    else
    {
        ImGui::TextDisabled("(select a shape)");
    }
    ImGui::End();
}

void SandboxLayer::render_scene(crd::rhi::CommandBuffer& cmd, crd::rhi::Image& sc_image, crd::u32 frame_index)
{
    const crd::rhi::Extent2D ext = m_frp->color_image().desc().extent;

    // Build camera matrices from smoothed quaternion orientation.
    const crd::math::Vec3f cam_offset = crd::math::rotate_vector(m_cam.q_smooth,
                                                                   crd::math::Vec3f{0.0F, 0.0F, m_cam.s_dist});
    const crd::math::Vec3f eye        = m_cam.s_target + cam_offset;
    const crd::math::Vec3f up_hint    = crd::math::rotate_vector(m_cam.q_smooth, crd::math::Vec3f{0.0F, 1.0F, 0.0F});

    const float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
    constexpr float kFovY  = 60.0F * (std::numbers::pi_v<float> / 180.0F);
    constexpr float kZNear = 0.01F;

    crd::renderer::FrameContext ctx;
    ctx.camera.view       = crd::math::look_at(eye, m_cam.s_target, up_hint);
    ctx.camera.projection = crd::math::perspective_reverse_z(kFovY, aspect, kZNear);
    ctx.camera_position   = eye;
    ctx.viewport          = ext;
    ctx.frame_index       = frame_index;

    // Submit mesh for solid rendering when solid mode is on.
    m_renderer.clear();
    const bool has_mesh = m_gpu_mesh.vertex_buffer != nullptr && m_last_uploaded >= 0;
    if (has_mesh && m_show_solid && m_surface_variant.is_valid())
    {
        crd::renderer::Renderable r;
        r.transform     = crd::math::Transformf::identity();
        r.vertex_buffer = m_gpu_mesh.vertex_buffer.get();
        r.vertex_count  = m_shapes[static_cast<crd::usize>(m_last_uploaded)].verts;
        r.index_buffer  = m_gpu_mesh.index_buffer.get();
        r.index_count   = m_shapes[static_cast<crd::usize>(m_last_uploaded)].indices;
        r.index_type    = crd::rhi::IndexType::Uint32;
        r.variant       = m_surface_variant;
        r.bucket        = crd::renderer::DrawBucket::Opaque;
        m_renderer.submit(r);
    }

    crd::renderer::DrawList draw_list;
    [[maybe_unused]] const bool build_ok = m_renderer.build_frame(ctx, *m_shader_runtime, draw_list);

    // Run ForwardRenderPath (always — clears and provides the background color even in wireframe-only mode).
    m_desc_alloc->begin_frame(frame_index % 2);
    crd::renderer::FrameGraph fg;
    m_frp->build(fg, draw_list, ctx);
    [[maybe_unused]] const bool ok = fg.build();
    CRD_ASSERT(ok);
    fg.execute(m_device, cmd);

    // Wireframe overlay — rendered on the FRP color image (still in ColorWrite after fg.execute()).
    if (m_show_wireframe && m_wf_pipeline && has_mesh)
    {
        const auto& shape = m_shapes[static_cast<crd::usize>(m_last_uploaded)];
        const crd::rhi::RenderingColorAttachmentInfo wf_att{
            &m_frp->color_image(), crd::rhi::LoadOp::Load, crd::rhi::StoreOp::Store, {}};
        cmd.begin_rendering({ext, wf_att, nullptr});
        cmd.set_viewport(ext);
        cmd.set_scissor({0, 0, ext.width, ext.height});
        cmd.bind_pipeline(*m_wf_pipeline);

        // MVP = view_proj * model; model is identity so MVP = view_proj.
        const crd::math::Mat4f mvp = ctx.camera.projection * ctx.camera.view;
        cmd.push_constants(*m_wf_layout, crd::rhi::ShaderStage::Vertex, 0U,
                           static_cast<crd::u32>(sizeof(crd::math::Mat4f)), &mvp);
        cmd.bind_vertex_buffer(*m_gpu_mesh.vertex_buffer, 0);
        if (m_gpu_mesh.index_buffer)
        {
            cmd.bind_index_buffer(*m_gpu_mesh.index_buffer, 0, crd::rhi::IndexType::Uint32);
            cmd.draw_indexed(shape.indices, 0, 0);
        }
        else
        {
            cmd.draw(shape.verts, 0);
        }
        cmd.end_rendering();
    }

    // Blit color render target to swapchain (ColorWrite → TransferSrc, then TransferDst → ColorWrite).
    cmd.transition_image(m_frp->color_image(), crd::rhi::ImageAccess::ColorWrite,
                         crd::rhi::ImageAccess::TransferSrc);
    cmd.transition_image(sc_image, crd::rhi::ImageAccess::Undefined,
                         crd::rhi::ImageAccess::TransferDst);
    cmd.blit_image(m_frp->color_image(), sc_image, ext, m_swapchain.desc().extent);
    // Leave swapchain in ColorWrite so ImGui can render with LoadOp::Load.
    cmd.transition_image(sc_image, crd::rhi::ImageAccess::TransferDst,
                         crd::rhi::ImageAccess::ColorWrite);
}

void SandboxLayer::on_event(crd::app::Event& event)
{
    crd::app::EventDispatcher dispatcher(event);
    dispatcher.dispatch<crd::app::WindowResizeEvent>([this](crd::app::WindowResizeEvent& e)
    {
        if (e.width() <= 0 || e.height() <= 0)
            return false; // minimized or invalid — skip

        m_device.wait_idle();
        const crd::rhi::Extent2D hint{static_cast<crd::u32>(e.width()),
                                      static_cast<crd::u32>(e.height())};
        m_swapchain.resize(hint);
        // Use the extent the swapchain actually settled on (clamped to caps.currentExtent).
        m_frp->resize(m_swapchain.desc().extent);
        return false; // don't consume — let other layers see the event too
    });
}

} // namespace crd::sandbox
