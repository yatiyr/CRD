#include "sandbox_layer.hpp"

#include <crd/app/event_dispatcher.hpp>
#include <crd/app/events/input_events.hpp>
#include <crd/app/events/window_events.hpp>
#include <crd/core/assert.hpp>
#include <crd/draw/draw.hpp>
#include <crd/draw/overlay_pass.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/draw_imgui/control_panel.hpp>
#include <crd/log/log.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>
#include <crd/meshgen/meshgen.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/input.hpp>
#include <crd/preset/preset_resolver.hpp>
#include <crd/profile/profile_resolver.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/mesh_resource_loader.hpp>
#include <crd/renderer/per_frame_data.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/shader/effect.hpp>
#include <crd/shader/shader_resource_loader.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

CRD_DEFINE_LOG_CHANNEL(g_log_sandbox_layer, "SandboxLayer", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

namespace crd::sandbox
{
namespace
{
constexpr float kOrbitSpeed  = 4.0F;
constexpr float kOrbitSens   = 0.05F;   // radians per mouse delta unit
constexpr float kPanSpeed    = 0.005F;
constexpr float kZoomSpeed   = 0.5F;
constexpr float kMinDistance = 0.1F;
constexpr float kMaxDistance = 500.0F;

// Read the UUID stored in a `.meta` sidecar produced by the asset cooker.
[[nodiscard]] crd::resources::ResourceId read_meta_uuid(const fs::Path& meta_path)
{
    crd::memory::MallocAllocator a;
    crd::containers::String text(&a);
    if (!fs::read_file_text(meta_path, text))
        return {};
    const std::string_view sv(text.data(), text.size());
    const std::string_view key = "uuid = \"";
    const auto pos = sv.find(key);
    if (pos == std::string_view::npos)
        return {};
    const auto start = pos + key.size();
    const auto end   = sv.find('"', start);
    if (end == std::string_view::npos)
        return {};
    return crd::resources::ResourceId::parse(sv.substr(start, end - start));
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
    m_compiled = true;
    if (handoff.modules.empty() || m_device == nullptr || m_layout == nullptr || m_runtime == nullptr)
        return;

    const crd::rhi::VertexBindingDesc binding{0, 48, crd::rhi::VertexInputRate::Vertex};
    const crd::rhi::VertexAttributeDesc attrs[4] = {
        {0, 0, crd::rhi::Format::R32G32B32Sfloat,    0},
        {1, 0, crd::rhi::Format::R32G32B32Sfloat,    12},
        {2, 0, crd::rhi::Format::R32G32Sfloat,       24},
        {3, 0, crd::rhi::Format::R32G32B32A32Sfloat, 32},
    };

    const crd::shader::Module* vert_module = nullptr;
    const crd::shader::Module* frag_module = nullptr;
    for (const auto& mod_usage : handoff.modules)
    {
        const crd::shader::Module* mod = m_runtime->find_module(mod_usage.module);
        if (mod == nullptr) continue;
        if (mod->stage() == crd::shader::Stage::Vertex)        vert_module = mod;
        else if (mod->stage() == crd::shader::Stage::Fragment) frag_module = mod;
    }
    if (vert_module == nullptr) return;

    const auto vert_bytes = vert_module->code_bytes();
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
    if (frag_module == nullptr) return;
    const auto frag_bytes = frag_module->code_bytes();
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
      m_assets(&m_alloc), m_profile_status(&m_alloc), m_obek_status(&m_alloc)
{
    register_procedural_assets();
    try_register_imported_assets();

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

    m_desc_alloc = device.create_descriptor_allocator({2, 512});
    CRD_ASSERT(m_desc_alloc != nullptr);

    const crd::rhi::Extent2D extent = swapchain.desc().extent;
    m_frp = crd::renderer::ForwardRenderPath::create(device, m_resolver, *m_desc_alloc, extent, 2);
    CRD_ASSERT(m_frp != nullptr);
    m_resolver.init(device, m_frp->pipeline_layout(), *m_shader_runtime, m_surface_variant, extent);

    build_wireframe_pipeline(source_dir);

    // Phase 3.0 v1o3: stand up the ECS World + component/index/system
    // registry. The boot-time Profile + Preset + Öbek loads kick from
    // the first on_update() call (deferred so the jobs system has been
    // initialised by Application::run() — load_async asserts on it).
    init_scene_world();
}

SandboxLayer::~SandboxLayer()
{
    // Wait for the GPU to drain so destroying entities + their GpuMesh
    // resources doesn't race with in-flight frames.
    m_device.wait_idle();

    // Tear down crd-draw before any owned RHI resources die.
    crd::draw::shutdown();

    // Drop the öbek instantiation FIRST so the world's destruction
    // doesn't double-free the öbek's source-payload back-reference.
    if (m_obek_instantiation)
    {
        if (m_world)
            m_world->unpack_obek(*m_obek_instantiation);
        m_obek_instantiation.reset();
    }

    // Tearing the world down fires on_remove for every Renderable on
    // every alive entity → RenderMeshIndex evicts every GpuMesh →
    // their unique_ptr<Buffer>s release. The order is important: the
    // World must outlive its registered indexes (it does, since they
    // are Array<unique_ptr<IComponentIndex>> members of World).
    m_world.reset();

    // Reap every async-load Counter our resource handles still own.
    // `load_async` acquires a Counter on each call; the consumer is
    // responsible for claiming it via `wait_ready()` (the handle
    // destructor only release_block()s — it doesn't reap counters).
    // If we skipped this, every handle that load-completed naturally
    // (state → Ready without an explicit wait_ready call) leaves an
    // acquired counter on the CounterPool and `jobs::shutdown()`
    // asserts on close. wait_ready is idempotent + returns
    // immediately for already-terminal states, so this is safe + cheap
    // even when the load finished long ago.
    // wait_ready is a no-op for handles that never bound a block
    // (returns Unloaded immediately); safe to call unconditionally.
    (void)m_pending_load.wait_ready();
    (void)m_quality_handle.wait_ready();
    (void)m_camera_handle.wait_ready();
    (void)m_profile_handle.wait_ready();
    (void)m_obek_handle.wait_ready();

    m_pending_load   = {};
    m_quality_handle = {};
    m_camera_handle  = {};
    m_profile_handle = {};
    m_obek_handle    = {};
}

void SandboxLayer::init_scene_world()
{
    m_world = std::make_unique<crd::scene::World>(&m_alloc);

    // Register Renderable with the AsyncAware trait so AsyncAwareIndex
    // auto-registers and the `query<>().skip_pending<Renderable>()` filter
    // is available. SparseSet storage hint matches what
    // RenderUploadSystem's tests use.
    m_world->register_component<crd::renderer::Renderable>(
        crd::scene::AsyncAware{},
        crd::scene::StorageHint::SparseSet);
    m_world->register_component<crd::renderer::PendingMeshUpload>(
        crd::scene::StorageHint::SparseSet);

    // Register the öbek-required built-ins: Transform (with the canonical
    // serialize trait so cooked OBEK's component bytes match by FourCC +
    // size) + TransformDirtyFlag + 6 built-in relations + the transform
    // propagation system. Mirrors the öbek cooker's `setup_temp_world`
    // (tools/asset_cooker/src/cook_handlers/obek.cpp) so cooked
    // components round-trip cleanly into the runtime World.
    m_world->register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    m_world->register_component<crd::scene::TransformDirtyFlag>(
        crd::scene::StorageHint::SparseSet);
    m_world->register_builtin_relations();
    m_world->register_system(std::make_unique<crd::scene::TransformPropagation>());

    // d3: DebugVizComponent + VisualizerRegistry + DebugVizSystem.
    // Component registration -> registry teaches the system how to draw
    // each component type -> system runs in PostRender phase, dispatches
    // every registered visualizer for every entity carrying DebugVizComponent.
    // The buffer is cleared in update() right before world.step() (see
    // SandboxLayer::on_update) so the system writes into a fresh buffer
    // every frame.
    m_world->register_component<crd::draw::DebugVizComponent>(
        crd::scene::StorageHint::SparseSet);
    crd::draw::register_default_visualizers(m_viz_registry);
    m_world->register_system(
        std::make_unique<crd::draw::DebugVizSystem>(m_viz_registry, m_draw_buffer));

    // d3 demo: spawn three entities at distinct positions so the
    // DebugVizSystem auto-emits an axis triad at each. set_translation
    // marks dirty so TransformPropagation populates Transform.world before
    // PostRender runs.
    for (const auto pos : {crd::math::Vec3f{4.0F, 0.0F, 0.0F},
                           crd::math::Vec3f{4.0F, 0.0F, 3.0F},
                           crd::math::Vec3f{4.0F, 0.0F, -3.0F}})
    {
        const auto e = m_world->spawn();
        m_world->add_component(e, crd::scene::Transform{});
        m_world->set_translation(e, pos);
        crd::draw::DebugVizComponent viz{};
        viz.scale = 0.5F;
        m_world->add_component(e, viz);
    }

    // RenderMeshIndex — observes Renderable's lifecycle and evicts GpuMeshes
    // automatically when entities are destroyed (the proper drop-callback
    // hook from v1o2's promise).
    m_mesh_idx = m_world->register_index<crd::renderer::RenderMeshIndex>(&m_alloc);
    m_mesh_idx->watch(m_world->component_id<crd::renderer::Renderable>());

    // RenderUploadSystem — promotes ready PendingMeshUpload entities in
    // the RenderExtract phase, calling RenderMeshIndex::install.
    m_world->register_system(std::make_unique<crd::renderer::RenderUploadSystem>());
}

void SandboxLayer::try_boot_profile_pipeline()
{
    m_profile_status = crd::containers::String(
        "Profile: not loaded (no resource manager)", &m_alloc);

    if (!m_resource_mgr) return;

    // Find the cooked default profile by relative path. The cooker uses
    // `path` strings for the manifest; we walk the manifest to find
    // anything that's a PROF artifact (assumed to be exactly one for
    // this slice) and load it.
    crd::resources::ResourceId profile_id;
    crd::resources::ResourceId quality_id;
    crd::resources::ResourceId camera_id;

    const fs::Path source_dir = fs::Path(CRD_SOURCE_DIR) / "assets/source";
    {
        const auto profile_meta = source_dir / "profiles/default.profile.toml.meta";
        profile_id = read_meta_uuid(profile_meta);
    }
    {
        const auto meta = source_dir / "presets/quality_default.preset.toml.meta";
        quality_id = read_meta_uuid(meta);
    }
    {
        const auto meta = source_dir / "presets/camera_default.preset.toml.meta";
        camera_id = read_meta_uuid(meta);
    }

    if (profile_id.is_null() || quality_id.is_null() || camera_id.is_null())
    {
        m_profile_status = crd::containers::String(
            "Profile: meta sidecars missing — re-cook demo assets", &m_alloc);
        CRD_LOG_WARN(g_log_sandbox_layer, "{}", m_profile_status.c_str());
        return;
    }

    // ResourceManager needs Profile + Preset loaders registered; v1n5/v1n2
    // ship public registrar helpers but to avoid pulling them in we use
    // the artifact builders' shared loader-construction pattern. Simpler:
    // register the loaders here.

    // (Loaders registered via free helper functions if exposed; fall back
    // to programmatic apply path if loaders aren't wired.)

    // Minimal v1o3: load Profile + Preset bytes synchronously and apply.
    m_profile_handle = m_resource_mgr->load_async<crd::profile::ProfileResource>(profile_id);
    m_quality_handle = m_resource_mgr->load_async<crd::preset::PresetResource>(quality_id);
    m_camera_handle  = m_resource_mgr->load_async<crd::preset::PresetResource>(camera_id);

    // Detect ProfileContext at boot.
    m_profile_context.os        = crd::profile::detect_os();
    m_profile_context.cpu_cores = crd::profile::detect_cpu_cores();

    // The actual apply happens in on_update once handles report Ready.
    m_profile_status = crd::containers::String(
        "Profile: load_async kicked; awaiting Ready", &m_alloc);
}

void SandboxLayer::try_load_demo_obek()
{
    if (!m_resource_mgr) return;
    const fs::Path source_dir = fs::Path(CRD_SOURCE_DIR) / "assets/source";
    const auto meta = source_dir / "obeks/obek_demo.obek.toml.meta";
    const auto id = read_meta_uuid(meta);
    if (id.is_null()) return;
    m_obek_handle = m_resource_mgr->load_async<crd::scene::ObekResource>(id);
    m_obek_status = crd::containers::String("Öbek: load_async kicked", &m_alloc);
}

void SandboxLayer::register_procedural_assets()
{
    crd::memory::MallocAllocator tmp;
    auto add = [&](const char* nm, crd::u32 idx, crd::renderer::MeshResource mesh)
    {
        AssetEntry e;
        e.display_name    = crd::containers::String(nm, &m_alloc);
        e.kind            = AssetKind::Procedural;
        e.procedural_idx  = idx;
        e.cached_verts    = mesh.primitives[0].vertex_count;
        e.cached_indices  = mesh.primitives[0].index_count;
        m_assets.push_back(std::move(e));
    };
    add("Plane",     0, crd::meshgen::make_plane(&tmp));
    add("Box",       1, crd::meshgen::make_box(&tmp));
    add("Sphere",    2, crd::meshgen::make_sphere(&tmp));
    add("Icosphere", 3, crd::meshgen::make_icosphere(&tmp));
    add("Cylinder",  4, crd::meshgen::make_cylinder(&tmp));
    add("Cone",      5, crd::meshgen::make_cone(&tmp));
    add("Capsule",   6, crd::meshgen::make_capsule(&tmp));
    add("Torus",     7, crd::meshgen::make_torus(&tmp));
}

void SandboxLayer::try_register_imported_assets()
{
#ifdef CRD_DEMO_ASSETS_REL_PACK
    const fs::Path exe_dir = fs::executable_dir();
    const fs::Path pack_path = exe_dir.empty()
        ? fs::Path(CRD_DEMO_ASSETS_REL_PACK)
        : exe_dir / crd::containers::StringView{CRD_DEMO_ASSETS_REL_PACK};
    if (!fs::is_file(pack_path))
    {
        CRD_LOG_WARN(g_log_sandbox_layer,
                     "Demo asset pack not found at '{}'. Run cook-demo-assets.",
                     pack_path.generic().data());
        return;
    }

    m_resource_mgr = std::make_unique<crd::resources::ResourceManager>(&m_alloc);
    crd::renderer::register_mesh_loader(m_resource_mgr.get());
    m_resource_mgr->register_loader(std::make_unique<crd::scene::ObekLoader>());

    // Register Profile + Preset loaders so the ResourceManager can resolve
    // PROF / PRQL / PRCM blobs.
    {
        auto loader = std::make_unique<crd::profile::ProfileLoader>(&m_alloc);
        m_resource_mgr->register_loader(std::move(loader));
    }
    {
        auto loader = std::make_unique<crd::preset::PresetLoader>(
            crd::preset::QualityPreset::fourcc,
            crd::preset::QualityPreset::version,
            static_cast<crd::u32>(sizeof(crd::preset::QualityPreset)),
            &m_alloc);
        m_resource_mgr->register_loader(std::move(loader));
    }
    {
        auto loader = std::make_unique<crd::preset::PresetLoader>(
            crd::preset::CameraPreset::fourcc,
            crd::preset::CameraPreset::version,
            static_cast<crd::u32>(sizeof(crd::preset::CameraPreset)),
            &m_alloc);
        m_resource_mgr->register_loader(std::move(loader));
    }

    const auto mount = m_resource_mgr->mount_manifest(pack_path.generic());
    if (!mount.is_valid())
    {
        CRD_LOG_ERROR(g_log_sandbox_layer, "Failed to mount '{}'", pack_path.generic().data());
        m_resource_mgr.reset();
        return;
    }

    const fs::Path source_dir = fs::Path(CRD_SOURCE_DIR) / "assets/source";
    struct ImportedDesc { const char* display; const char* glb_filename; };
    const ImportedDesc imports[] = {
        {"BoxTextured (glTF)", "BoxTextured.glb"},
        {"Duck (glTF)",        "Duck.glb"},
        {"BoomBox (glTF)",     "BoomBox.glb"},
    };
    for (const auto& imp : imports)
    {
        const fs::Path glb_path  = source_dir / imp.glb_filename;
        crd::containers::String meta_str(&m_alloc);
        meta_str.append(glb_path.generic().data(), glb_path.generic().size());
        meta_str.append(".meta");
        const fs::Path meta_path(crd::containers::StringView(meta_str.data(), meta_str.size()));
        if (!fs::is_file(meta_path)) continue;
        const auto id = read_meta_uuid(meta_path);
        if (id.is_null()) continue;
        if (m_resource_mgr->find_entry(id) == nullptr) continue;
        AssetEntry e;
        e.display_name = crd::containers::String(imp.display, &m_alloc);
        e.kind         = AssetKind::Imported;
        e.imported_id  = id;
        m_assets.push_back(std::move(e));
    }
    m_imported_available = true;
    CRD_LOG_INFO(g_log_sandbox_layer, "Mounted '{}'", pack_path.generic().data());
#endif

    // crd-draw integration (v1a-draw d0d).
    // 1. Register the SHDR loader so the ResourceManager can deserialise
    //    cooked shader resources (line_aa.vert.glsl + line_aa.frag.glsl).
    // 2. Mount the draw_shaders.crdr pack copied next to this exe by the
    //    sandbox CMake's `sandbox-draw-pack` target.
    // 3. Initialise the renderer + line pipeline.
    crd::shader::register_shader_loader(m_resource_mgr.get());

    const fs::Path draw_pack_path = fs::executable_dir() / CRD_DRAW_SHADERS_REL_PACK;
    if (!fs::is_file(draw_pack_path))
    {
        CRD_LOG_WARN(g_log_sandbox_layer, "crd-draw pack not found at '{}'; debug overlay disabled",
                     draw_pack_path.generic().data());
    }
    else
    {
        const auto draw_mount = m_resource_mgr->mount_manifest(draw_pack_path.generic());
        if (!draw_mount.is_valid())
        {
            CRD_LOG_WARN(g_log_sandbox_layer, "Failed to mount crd-draw pack '{}'; debug overlay disabled",
                         draw_pack_path.generic().data());
        }
        else
        {
            crd::draw::InitConfig draw_cfg{};
            draw_cfg.color_format            = crd::rhi::Format::B8G8R8A8Unorm;
            draw_cfg.depth_format            = crd::rhi::Format::D32Sfloat;
            draw_cfg.frames_in_flight        = 2;
            draw_cfg.max_lines_per_frame     = 4096;
            draw_cfg.max_triangles_per_frame = 4096;
            if (!crd::draw::init(*m_resource_mgr, m_device, draw_cfg))
            {
                CRD_LOG_WARN(g_log_sandbox_layer, "crd::draw::init failed; debug overlay disabled");
            }
            else
            {
                CRD_LOG_INFO(g_log_sandbox_layer, "crd-draw initialised (overlay path live)");
                // d4: prime the persistent OverlayPassConfig with the
                // active theme + grid defaults. The ImGui control panel
                // mutates m_draw_cfg from here on; per-frame fields
                // (view_proj, viewport_px, frame_in_flight_index,
                // camera_pos) are written every frame in render_scene.
                m_draw_cfg.grid.apply_theme();
                m_draw_cfg.grid.enabled = true;
                m_draw_cfg.grid.plane_y = -1.0F;
            }
        }
    }
}

void SandboxLayer::build_wireframe_pipeline(const crd::platform::fs::Path& source_dir)
{
    const crd::rhi::PushConstantRange wf_push{crd::rhi::ShaderStage::Vertex, 0, 64U};
    m_wf_layout = m_device.create_pipeline_layout({
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>{},
        crd::containers::make_span(&wf_push, 1U)});
    if (!m_wf_layout) return;

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
    if (!wf_variant.is_valid()) return;
    const crd::shader::Module* vert_mod = nullptr;
    const crd::shader::Module* frag_mod = nullptr;
    for (const auto mh : m_shader_runtime->variant_modules(wf_variant))
    {
        const crd::shader::Module* mod = m_shader_runtime->find_module(mh);
        if (mod == nullptr) continue;
        if (mod->stage() == crd::shader::Stage::Vertex)        vert_mod = mod;
        else if (mod->stage() == crd::shader::Stage::Fragment) frag_mod = mod;
    }
    if (vert_mod == nullptr || frag_mod == nullptr) return;
    const auto vert_bytes = vert_mod->code_bytes();
    const auto frag_bytes = frag_mod->code_bytes();
    auto vk_vert = m_device.create_shader_module(
        {crd::rhi::ShaderStage::Vertex, "main",
         crd::containers::make_span(vert_bytes.data(), vert_bytes.size())});
    auto vk_frag = m_device.create_shader_module(
        {crd::rhi::ShaderStage::Fragment, "main",
         crd::containers::make_span(frag_bytes.data(), frag_bytes.size())});
    if (!vk_vert || !vk_frag) return;
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
}

// ── camera + ECS update ────────────────────────────────────────────────────

void SandboxLayer::on_update(crd::f64 delta_seconds)
{
    // Deferred boot — load_async requires `crd::jobs::init()` which
    // Application::run() invokes before the first on_update tick.
    if (!m_boot_kicked)
    {
        try_boot_profile_pipeline();
        try_load_demo_obek();
        m_boot_kicked = true;
    }

    const auto& input = m_app.window().input().state();
    const float dt    = static_cast<float>(delta_seconds);
    const bool imgui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

    if (!imgui_wants_mouse)
    {
        if (input.is_mouse_down(crd::platform::MouseButton::Left))
        {
            const auto q_yaw   = crd::math::from_axis_angle(crd::math::Vec3f{0.0F, 1.0F, 0.0F},
                                                              -input.mouse_dx() * kOrbitSens);
            const auto q_pitch = crd::math::from_axis_angle(crd::math::Vec3f{1.0F, 0.0F, 0.0F},
                                                              -input.mouse_dy() * kOrbitSens);
            m_cam.q_target = crd::math::normalized(q_yaw * m_cam.q_target * q_pitch);
        }
        if (input.is_mouse_down(crd::platform::MouseButton::Middle) &&
            (input.is_key_down(crd::platform::Key::LeftCtrl) || input.is_key_down(crd::platform::Key::RightCtrl)))
        {
            const auto cam_right = crd::math::rotate_vector(m_cam.q_target, crd::math::Vec3f{1.0F, 0.0F, 0.0F});
            const auto cam_up    = crd::math::rotate_vector(m_cam.q_target, crd::math::Vec3f{0.0F, 1.0F, 0.0F});
            const float scale    = m_cam.distance * kPanSpeed;
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

    // Orientation accumulates continuously into q_target (every mouse
    // delta contributes during input poll above), then q_smooth slerps
    // toward it with the same exponential smoothing the distance + pan
    // target use. Effect: motion is buttery while the user drags AND
    // decays smoothly to a stop when they release — no jitter, no
    // hard snap on release.
    const float smooth_t = 1.0F - std::exp(-kOrbitSpeed * dt);
    m_cam.q_smooth = crd::math::slerp(m_cam.q_smooth, m_cam.q_target, smooth_t);
    m_cam.s_dist   = crd::math::damp(m_cam.s_dist,   m_cam.distance, kOrbitSpeed, dt);
    m_cam.s_target = crd::math::damp(m_cam.s_target, m_cam.target,   kOrbitSpeed, dt);

    // Profile / Preset handles — apply once they reach Ready.
    if (!m_profile_applied && m_quality_handle.state() == crd::resources::LoadState::Ready
        && m_camera_handle.state() == crd::resources::LoadState::Ready
        && m_profile_handle.state() == crd::resources::LoadState::Ready)
    {
        const crd::preset::PresetResource* q_res = m_quality_handle.get();
        const crd::preset::PresetResource* c_res = m_camera_handle.get();
        if (q_res != nullptr && c_res != nullptr)
        {
            // Seed runtime sliders from the cooked values so the Quality /
            // Camera panel starts at the resolved defaults.
            std::memcpy(&m_quality_runtime, q_res->bytes().data(), sizeof(m_quality_runtime));
            std::memcpy(&m_camera_runtime,  c_res->bytes().data(), sizeof(m_camera_runtime));

            // Apply L0..L2 to targets via the resolver. With no L4 override
            // for boot, the resolved value is the cooked value itself.
            crd::preset::apply_preset<crd::preset::QualityPreset>(*m_frp,            q_res);
            crd::preset::apply_preset<crd::preset::CameraPreset>(m_camera_target,    c_res);

            m_profile_applied = true;
            m_profile_status = crd::containers::String(
                "Profile: applied (Quality + Camera)", &m_alloc);
        }
    }

    // Re-apply runtime overrides when sliders dirty.
    if (m_quality_runtime_dirty)
    {
        crd::preset::apply_preset<crd::preset::QualityPreset>(*m_frp, m_quality_handle.get(),
                                                              &m_quality_runtime);
        m_quality_runtime_dirty = false;
    }
    if (m_camera_runtime_dirty)
    {
        crd::preset::apply_preset<crd::preset::CameraPreset>(m_camera_target, m_camera_handle.get(),
                                                             &m_camera_runtime);
        m_camera_runtime_dirty = false;
    }

    // Öbek instantiate-once when the resource handle reaches Ready.
    if (!m_obek_loaded && m_obek_handle.state() == crd::resources::LoadState::Ready
        && m_world)
    {
        const crd::scene::ObekResource* obek = m_obek_handle.get();
        if (obek != nullptr)
        {
            m_obek_instantiation = std::make_unique<crd::scene::ObekInstantiation>(
                m_world->instantiate_obek(*obek));
            m_obek_loaded = true;
            m_obek_status = crd::containers::String(
                "Öbek: instantiated (root + child)", &m_alloc);
        }
    }

    // Re-spawn the selected asset's entity if selection changed.
    const int target_index = (m_pending_index >= 0) ? m_pending_index : m_last_displayed;
    const bool selection_changed = m_selected != target_index;
    const bool params_changed    = m_mesh_dirty;
    if ((selection_changed || params_changed) &&
        m_selected >= 0 && m_selected < static_cast<int>(m_assets.size()))
    {
        select_asset(m_selected);
        m_mesh_dirty = false;
    }

    try_finalize_pending_load();

    // d3: clear the draw buffer BEFORE world.step so DebugVizSystem
    // (PostRender phase) writes into a fresh buffer. Showroom emissions
    // in render_scene() append to the same buffer afterwards. The
    // overlay-pass at end-of-render submits the merged buffer.
    if (crd::draw::is_initialised())
    {
        m_draw_buffer.clear();
    }

    // Drive the schedule so RenderUploadSystem promotes pending uploads.
    if (m_world) m_world->step(delta_seconds);
}

void SandboxLayer::select_asset(int idx)
{
    const AssetEntry& entry = m_assets[static_cast<crd::usize>(idx)];
    if (entry.kind == AssetKind::Procedural)
    {
        m_pending_load  = {};
        m_pending_index = -1;
        respawn_procedural(idx);
    }
    else
    {
        kick_async_import(idx);
    }
}

void SandboxLayer::destroy_current_entity_if_any()
{
    if (m_world && !m_current_entity.is_null())
    {
        m_world->destroy(m_current_entity);
        m_world->flush_destroys();
        m_current_entity = crd::scene::EntityId::null();
    }
}

void SandboxLayer::respawn_procedural(int idx)
{
    if (!m_world) return;

    m_device.wait_idle(); // safe: prior frame complete before freeing old buffers
    destroy_current_entity_if_any();

    AssetEntry& entry = m_assets[static_cast<crd::usize>(idx)];
    crd::memory::MallocAllocator tmp;
    crd::renderer::MeshResource cpu_mesh(&tmp);
    switch (entry.procedural_idx)
    {
    case 0: cpu_mesh = crd::meshgen::make_plane    (&tmp, m_plane.w, m_plane.d,
                                                    static_cast<crd::u32>(m_plane.divs_x),
                                                    static_cast<crd::u32>(m_plane.divs_z)); break;
    case 1: cpu_mesh = crd::meshgen::make_box      (&tmp, m_box.w, m_box.h, m_box.d); break;
    case 2: cpu_mesh = crd::meshgen::make_sphere   (&tmp, m_sphere.radius,
                                                    static_cast<crd::u32>(m_sphere.lat),
                                                    static_cast<crd::u32>(m_sphere.lon)); break;
    case 3: cpu_mesh = crd::meshgen::make_icosphere(&tmp, m_ico.radius,
                                                    static_cast<crd::u32>(m_ico.subdiv)); break;
    case 4: cpu_mesh = crd::meshgen::make_cylinder (&tmp, m_cylinder.radius, m_cylinder.h,
                                                    static_cast<crd::u32>(m_cylinder.segs)); break;
    case 5: cpu_mesh = crd::meshgen::make_cone     (&tmp, m_cone.radius, m_cone.h,
                                                    static_cast<crd::u32>(m_cone.segs)); break;
    case 6: cpu_mesh = crd::meshgen::make_capsule  (&tmp, m_capsule.radius, m_capsule.h,
                                                    static_cast<crd::u32>(m_capsule.segs),
                                                    static_cast<crd::u32>(m_capsule.rings)); break;
    case 7: cpu_mesh = crd::meshgen::make_torus    (&tmp, m_torus.maj_r, m_torus.min_r,
                                                    static_cast<crd::u32>(m_torus.maj_segs),
                                                    static_cast<crd::u32>(m_torus.min_segs)); break;
    default: return;
    }
    entry.cached_verts   = cpu_mesh.primitives[0].vertex_count;
    entry.cached_indices = cpu_mesh.primitives[0].index_count;

    auto gpu_mesh = crd::renderer::GpuUploader::upload_mesh(cpu_mesh, m_device);

    crd::renderer::Renderable r;
    r.transform     = crd::math::Transformf::identity();
    r.vertex_buffer = gpu_mesh.vertex_buffer.get();
    r.vertex_count  = entry.cached_verts;
    r.index_buffer  = gpu_mesh.index_buffer.get();
    r.index_count   = entry.cached_indices;
    r.index_type    = crd::rhi::IndexType::Uint32;
    r.variant       = m_surface_variant;
    r.bucket        = crd::renderer::DrawBucket::Opaque;

    m_current_entity = m_world->spawn();
    m_world->add_component<crd::renderer::Renderable>(m_current_entity, std::move(r));
    [[maybe_unused]] const bool inserted =
        m_mesh_idx->install(m_current_entity, std::move(gpu_mesh));
    CRD_ASSERT(inserted);

    // Procedurals are sync-uploaded; mark AsyncAware Loaded immediately so
    // skip_pending<Renderable>() doesn't filter them out.
    if (auto* aa = m_world->find_index<crd::scene::AsyncAwareIndex>())
        aa->mark_loaded(m_current_entity, m_world->component_id<crd::renderer::Renderable>());

    m_last_displayed = idx;
    CRD_LOG_INFO(g_log_sandbox_layer, "Spawned procedural '{}': {}v {}i",
                 entry.display_name.c_str(), entry.cached_verts, entry.cached_indices);
}

void SandboxLayer::kick_async_import(int idx)
{
    if (!m_resource_mgr) return;
    const AssetEntry& entry = m_assets[static_cast<crd::usize>(idx)];
    m_pending_load  = m_resource_mgr->load_async<crd::renderer::MeshResource>(entry.imported_id);
    m_pending_index = idx;
    CRD_LOG_TRACE(g_log_sandbox_layer, "Kicked async load for '{}'",
                  entry.display_name.c_str());
}

void SandboxLayer::try_finalize_pending_load()
{
    if (m_pending_index < 0) return;
    const auto state = m_pending_load.state();
    if (state != crd::resources::LoadState::Ready &&
        state != crd::resources::LoadState::Failed &&
        state != crd::resources::LoadState::Placeholder) return;

    if (state == crd::resources::LoadState::Failed)
    {
        m_pending_load = {};
        m_pending_index = -1;
        return;
    }

    AssetEntry& entry = m_assets[static_cast<crd::usize>(m_pending_index)];
    const crd::renderer::MeshResource* cpu = m_pending_load.get();
    if (cpu == nullptr || cpu->primitives.empty()
        || cpu->vertices.empty() || cpu->indices.empty())
    {
        m_pending_load = {};
        m_pending_index = -1;
        return;
    }

    crd::u32 total_verts = 0, total_idx = 0;
    for (const auto& p : cpu->primitives) { total_verts += p.vertex_count; total_idx += p.index_count; }
    entry.cached_verts   = total_verts;
    entry.cached_indices = total_idx;

    m_device.wait_idle();
    destroy_current_entity_if_any();

    // Async path: kick upload_mesh_async (returns UploadHandle owning fence
    // + staging + pending GpuMesh). Spawn an entity with Renderable
    // (buffers nullptr until promotion) + PendingMeshUpload{handle}.
    auto handle = crd::renderer::GpuUploader::upload_mesh_async(*cpu, m_device);

    m_current_entity = m_world->spawn();

    crd::renderer::Renderable r;
    r.transform    = crd::math::Transformf::identity();
    r.vertex_count = total_verts;
    r.index_count  = total_idx;
    r.index_type   = crd::rhi::IndexType::Uint32;
    r.variant      = m_surface_variant;
    r.bucket       = crd::renderer::DrawBucket::Opaque;
    m_world->add_component<crd::renderer::Renderable>(m_current_entity, std::move(r));
    m_world->add_component<crd::renderer::PendingMeshUpload>(
        m_current_entity, crd::renderer::PendingMeshUpload{std::move(handle)});

    m_last_displayed = m_pending_index;
    CRD_LOG_INFO(g_log_sandbox_layer, "Spawned imported '{}' (async): {}v {}i",
                 entry.display_name.c_str(), total_verts, total_idx);

    m_pending_load = {};
    m_pending_index = -1;
}

void SandboxLayer::apply_obek_translation_override()
{
    if (!m_world || !m_obek_instantiation) return;
    // The öbek's "child" entity is at file_idx 0 or 1; ObekInstantiation
    // exposes a flat array. v1o3 uses a name lookup helper: the child has
    // ChildOf relation to root; we apply translation override to whichever
    // entity *has* a parent (i.e. is not a root).
    const auto& inst = *m_obek_instantiation;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(inst.entities.size()); ++i)
    {
        const auto e = inst.entities[i];
        if (m_world->has_relation<crd::scene::relations::ChildOf>(e))
        {
            m_world->set_translation(e, m_obek_child_override_translation);
            m_obek_child_override_active = true;
            m_obek_status = crd::containers::String(
                "Öbek: child translation override applied", &m_alloc);
            return;
        }
    }
}

void SandboxLayer::revert_obek_translation_override()
{
    if (!m_world || !m_obek_instantiation) return;
    const auto& inst = *m_obek_instantiation;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(inst.entities.size()); ++i)
    {
        const auto e = inst.entities[i];
        if (m_world->has_relation<crd::scene::relations::ChildOf>(e))
        {
            // Revert the entire Transform component (component_fourcc =
            // Transform's serialize FourCC, captured from registry).
            const auto cid = m_world->component_id<crd::scene::Transform>();
            if (const auto* info = m_world->component_info(cid))
            {
                m_world->revert_component(*m_obek_instantiation, i,
                                          info->serialize.fourcc);
                m_obek_child_override_active = false;
                m_obek_status = crd::containers::String(
                    "Öbek: child translation reverted", &m_alloc);
            }
            return;
        }
    }
}

void SandboxLayer::unpack_obek_instantiation()
{
    if (!m_world || !m_obek_instantiation) return;
    m_world->unpack_obek(*m_obek_instantiation);
    m_obek_status = crd::containers::String(
        "Öbek: unpacked (entities now plain World data)", &m_alloc);
}

// ── render ─────────────────────────────────────────────────────────────────

void SandboxLayer::on_render()
{
    const auto& ext = m_swapchain.desc().extent;

    // ── Sandbox status ────────────────────────────────────────────────────
    ImGui::SetNextWindowPos({8.0F, 8.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({340.0F, 280.0F}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Sandbox", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Viewport: %u x %u", ext.width, ext.height);
    ImGui::Separator();
    ImGui::TextDisabled("Camera (smoothed)");
    ImGui::Text("  distance: %.2f", static_cast<double>(m_cam.s_dist));
    ImGui::Text("  target:   (%.2f, %.2f, %.2f)",
                static_cast<double>(m_cam.s_target.x),
                static_cast<double>(m_cam.s_target.y),
                static_cast<double>(m_cam.s_target.z));
    ImGui::Separator();
    ImGui::TextDisabled("ECS");
    if (m_world)
    {
        ImGui::Text("  entities:        %u", static_cast<unsigned>(m_world->entity_count()));
        ImGui::Text("  meshes resident: %zu", m_mesh_idx ? m_mesh_idx->count() : 0U);
        crd::usize pending = 0;
        if (m_world->component_id<crd::renderer::PendingMeshUpload>().raw != 0xFFFFU)
        {
            for (auto&& [e, pm] : m_world->query<crd::renderer::PendingMeshUpload>())
            {
                (void)e; (void)pm; ++pending;
            }
        }
        ImGui::Text("  pending uploads: %zu", pending);
    }
    ImGui::Separator();
    ImGui::TextDisabled("LMB drag=orbit  Ctrl+MMB=pan  Scroll=zoom");
    ImGui::End();

    // ── Profile + Quality + Camera ────────────────────────────────────────
    ImGui::SetNextWindowPos({8.0F, 296.0F}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({340.0F, 360.0F}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Profile / Presets", nullptr);
    ImGui::TextDisabled("ProfileContext (boot-time)");
    ImGui::Text("  os:        %d", static_cast<int>(m_profile_context.os));
    ImGui::Text("  cpu_cores: %d", m_profile_context.cpu_cores);
    ImGui::Separator();
    ImGui::Text("%s", m_profile_status.c_str());
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Quality preset (L4 runtime override)",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool changed = false;
        int sr = static_cast<int>(m_quality_runtime.shadow_resolution);
        if (ImGui::SliderInt("shadow_resolution", &sr, 256, 8192))
        {
            m_quality_runtime.shadow_resolution = static_cast<crd::u32>(sr);
            changed = true;
        }
        int msaa = static_cast<int>(m_quality_runtime.msaa_samples);
        if (ImGui::SliderInt("msaa_samples", &msaa, 1, 8))
        {
            m_quality_runtime.msaa_samples = static_cast<crd::u8>(msaa);
            changed = true;
        }
        bool dpp = m_quality_runtime.enable_depth_prepass != 0U;
        if (ImGui::Checkbox("enable_depth_prepass", &dpp))
        {
            m_quality_runtime.enable_depth_prepass = dpp ? 1U : 0U;
            changed = true;
        }
        if (changed) m_quality_runtime_dirty = true;
        ImGui::TextDisabled("FRP cached: shadow=%u, msaa=%u, prepass=%u",
                            m_frp->quality_preset().shadow_resolution,
                            m_frp->quality_preset().msaa_samples,
                            m_frp->quality_preset().enable_depth_prepass);
    }
    if (ImGui::CollapsingHeader("Camera preset (L4 runtime override)",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool changed = false;
        if (ImGui::SliderFloat("fov_y_radians", &m_camera_runtime.fov_y_radians,
                                0.2F, 2.5F)) changed = true;
        if (ImGui::SliderFloat("near_plane",    &m_camera_runtime.near_plane,
                                0.001F, 1.0F))  changed = true;
        if (ImGui::SliderFloat("far_plane",     &m_camera_runtime.far_plane,
                                10.0F, 5000.0F)) changed = true;
        if (changed) m_camera_runtime_dirty = true;
        ImGui::TextDisabled("Target cached: fov=%.3f near=%.3f far=%.1f (apply_count=%u)",
                            static_cast<double>(m_camera_target.preset().fov_y_radians),
                            static_cast<double>(m_camera_target.preset().near_plane),
                            static_cast<double>(m_camera_target.preset().far_plane),
                            m_camera_target.apply_count());
    }
    ImGui::End();

    // ── Öbek panel ─────────────────────────────────────────────────────────
    ImGui::SetNextWindowPos({8.0F, 664.0F}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({340.0F, 220.0F}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Öbek demo", nullptr);
    ImGui::Text("%s", m_obek_status.c_str());
    if (m_obek_loaded && m_obek_instantiation)
    {
        ImGui::Separator();
        ImGui::Text("Entities: %zu", m_obek_instantiation->entities.size());
        ImGui::SliderFloat3("child override translation",
                             &m_obek_child_override_translation.x, -5.0F, 5.0F);
        if (ImGui::Button("Apply translation override"))
            apply_obek_translation_override();
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
            revert_obek_translation_override();
        ImGui::SameLine();
        if (ImGui::Button("Unpack"))
            unpack_obek_instantiation();
        ImGui::TextDisabled("Override active: %s",
                            m_obek_child_override_active ? "yes" : "no");

        // Display child entity's current translation (read-back).
        for (crd::u32 i = 0; i < static_cast<crd::u32>(m_obek_instantiation->entities.size()); ++i)
        {
            const auto e = m_obek_instantiation->entities[i];
            if (m_world && m_world->has_relation<crd::scene::relations::ChildOf>(e))
            {
                if (const auto* t = m_world->get_component<crd::scene::Transform>(e))
                {
                    ImGui::Text("Child translation: (%.2f, %.2f, %.2f)",
                                static_cast<double>(t->translation.x),
                                static_cast<double>(t->translation.y),
                                static_cast<double>(t->translation.z));
                }
                break;
            }
        }
    }
    ImGui::End();

    // ── Asset Browser ──────────────────────────────────────────────────────
    ImGui::SetNextWindowPos({356.0F, 8.0F}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({340.0F, 580.0F}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Asset Browser", nullptr);
    ImGui::Text("Render Mode");
    ImGui::Checkbox("Solid",     &m_show_solid);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &m_show_wireframe);
    ImGui::Separator();

    crd::u32 procedural_count = 0, imported_count = 0;
    for (const auto& a : m_assets)
    {
        if (a.kind == AssetKind::Procedural) ++procedural_count;
        else                                 ++imported_count;
    }

    if (ImGui::CollapsingHeader("Procedural Shapes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("(%u)", procedural_count);
        ImGui::Indent();
        for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
        {
            const auto& a = m_assets[static_cast<crd::usize>(i)];
            if (a.kind != AssetKind::Procedural) continue;
            if (ImGui::Selectable(a.display_name.c_str(), m_selected == i))
                m_selected = i;
        }
        ImGui::Unindent();
    }
    if (m_imported_available && ImGui::CollapsingHeader("Imported Assets",
                                                         ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("(%u)", imported_count);
        ImGui::Indent();
        if (imported_count == 0)
            ImGui::TextDisabled("(no imports cooked — run cook-demo-assets)");
        for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
        {
            const auto& a = m_assets[static_cast<crd::usize>(i)];
            if (a.kind != AssetKind::Imported) continue;
            if (ImGui::Selectable(a.display_name.c_str(), m_selected == i))
                m_selected = i;
        }
        ImGui::Unindent();
    }

    ImGui::Separator();
    if (m_selected >= 0 && m_selected < static_cast<int>(m_assets.size()))
    {
        const auto& a = m_assets[static_cast<crd::usize>(m_selected)];
        const bool is_loading = (m_pending_index == m_selected);
        ImGui::Text("Name:    %s", a.display_name.c_str());
        ImGui::Text("Source:  %s", a.kind == AssetKind::Procedural ? "Procedural" : "glTF");
        if (is_loading)
            ImGui::TextColored({0.9F, 0.7F, 0.2F, 1.0F}, "Status:  loading...");
        else
        {
            ImGui::Text("Verts:   %u", a.cached_verts);
            ImGui::Text("Indices: %u", a.cached_indices);
            ImGui::Text("Tris:    %u", a.cached_indices / 3U);
        }
        if (a.kind == AssetKind::Procedural)
        {
            ImGui::Separator();
            auto dirty = [this]() { if (ImGui::IsItemDeactivatedAfterEdit()) m_mesh_dirty = true; };
            switch (a.procedural_idx)
            {
            case 0: ImGui::SliderFloat("Width##pl", &m_plane.w, 0.1F, 10.0F); dirty();
                    ImGui::SliderFloat("Depth##pl", &m_plane.d, 0.1F, 10.0F); dirty();
                    ImGui::SliderInt("Divs X##pl", &m_plane.divs_x, 1, 32);   dirty();
                    ImGui::SliderInt("Divs Z##pl", &m_plane.divs_z, 1, 32);   dirty(); break;
            case 1: ImGui::SliderFloat("Width##bx",  &m_box.w, 0.1F, 5.0F); dirty();
                    ImGui::SliderFloat("Height##bx", &m_box.h, 0.1F, 5.0F); dirty();
                    ImGui::SliderFloat("Depth##bx",  &m_box.d, 0.1F, 5.0F); dirty(); break;
            case 2: ImGui::SliderFloat("Radius##sp",  &m_sphere.radius, 0.1F, 5.0F); dirty();
                    ImGui::SliderInt("Lat bands##sp", &m_sphere.lat,    4, 64);      dirty();
                    ImGui::SliderInt("Lon bands##sp", &m_sphere.lon,    4, 64);      dirty(); break;
            case 3: ImGui::SliderFloat("Radius##ic",     &m_ico.radius, 0.1F, 5.0F); dirty();
                    ImGui::SliderInt("Subdivisions##ic", &m_ico.subdiv, 0, 5);       dirty(); break;
            case 4: ImGui::SliderFloat("Radius##cy", &m_cylinder.radius, 0.05F, 3.0F); dirty();
                    ImGui::SliderFloat("Height##cy", &m_cylinder.h,      0.1F,  5.0F); dirty();
                    ImGui::SliderInt("Segments##cy", &m_cylinder.segs,   3, 64);       dirty(); break;
            case 5: ImGui::SliderFloat("Radius##co", &m_cone.radius, 0.05F, 3.0F); dirty();
                    ImGui::SliderFloat("Height##co", &m_cone.h,      0.1F,  5.0F); dirty();
                    ImGui::SliderInt("Segments##co", &m_cone.segs,   3, 64);       dirty(); break;
            case 6: ImGui::SliderFloat("Radius##ca", &m_capsule.radius, 0.05F, 3.0F); dirty();
                    ImGui::SliderFloat("Height##ca", &m_capsule.h,      0.1F,  5.0F); dirty();
                    ImGui::SliderInt("Segments##ca", &m_capsule.segs,   3, 64);       dirty();
                    ImGui::SliderInt("Rings##ca",    &m_capsule.rings,  2, 16);       dirty(); break;
            case 7: ImGui::SliderFloat("Major R##to", &m_torus.maj_r,    0.2F, 5.0F); dirty();
                    ImGui::SliderFloat("Minor R##to", &m_torus.min_r,    0.05F, 2.0F); dirty();
                    ImGui::SliderInt("Maj segs##to",  &m_torus.maj_segs, 4, 64);      dirty();
                    ImGui::SliderInt("Min segs##to",  &m_torus.min_segs, 4, 32);      dirty(); break;
            default: break;
            }
        }
    }
    else
        ImGui::TextDisabled("(select an asset)");
    ImGui::End();

    // d4: crd-draw control panel. Mutates m_draw_cfg + the crd-draw
    // process globals (set_overlay_enabled, set_theme); changes take
    // effect on the next submitted frame.
    if (crd::draw::is_initialised())
    {
        crd::draw_imgui::draw_control_panel(m_draw_cfg);
    }
}

void SandboxLayer::render_scene(crd::rhi::CommandBuffer& cmd, crd::rhi::Image& sc_image,
                                crd::u32 frame_index)
{
    const crd::rhi::Extent2D ext = m_frp->color_image().desc().extent;

    // Build camera matrices, driven by the resolved CameraPreset (FOV /
    // near plane). Far plane intentionally honours the preset too.
    const crd::math::Vec3f cam_offset = crd::math::rotate_vector(
        m_cam.q_smooth, crd::math::Vec3f{0.0F, 0.0F, m_cam.s_dist});
    const crd::math::Vec3f eye     = m_cam.s_target + cam_offset;
    const crd::math::Vec3f up_hint = crd::math::rotate_vector(m_cam.q_smooth,
                                                               crd::math::Vec3f{0.0F, 1.0F, 0.0F});
    const float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
    const auto& cam_preset = m_camera_target.preset();

    crd::renderer::FrameContext ctx;
    ctx.camera.view       = crd::math::look_at(eye, m_cam.s_target, up_hint);
    ctx.camera.projection = crd::math::perspective_reverse_z(
        cam_preset.fov_y_radians, aspect, cam_preset.near_plane);
    ctx.camera_position   = eye;
    ctx.viewport          = ext;
    ctx.frame_index       = frame_index;

    // Submit every alive Renderable to the renderer. .skip_pending<>
    // filters entities still mid-async-upload; procedural-spawned
    // entities are flagged Loaded immediately so they pass.
    m_renderer.clear();
    if (m_world && m_show_solid && m_surface_variant.is_valid())
    {
        for (auto&& [entity, r] : m_world->query<crd::renderer::Renderable>()
                                       .skip_pending<crd::renderer::Renderable>())
        {
            (void)entity;
            if (r.vertex_buffer == nullptr) continue; // not yet promoted
            crd::renderer::Renderable tmp = r;
            tmp.variant = m_surface_variant;
            m_renderer.submit(tmp);
        }
    }

    crd::renderer::DrawList draw_list;
    [[maybe_unused]] const bool build_ok = m_renderer.build_frame(ctx, *m_shader_runtime, draw_list);

    m_desc_alloc->begin_frame(frame_index % 2);
    crd::renderer::FrameGraph fg;
    m_frp->build(fg, draw_list, ctx);
    [[maybe_unused]] const bool ok = fg.build();
    CRD_ASSERT(ok);
    fg.execute(m_device, cmd);

    // crd-draw overlay (v1a-draw d0d). Build a tiny demo set: an axis triad
    // at origin + a wire box per spawned entity at a fixed offset. Then
    // graft an overlay pass onto a fresh FrameGraph so add_draw_overlay_pass
    // exercises the full pipeline path.
    if (crd::draw::is_initialised())
    {
        // d3: m_draw_buffer is cleared at the top of on_update (BEFORE
        // world.step) so DebugVizSystem (PostRender) writes into a fresh
        // buffer; showroom emissions below append to the same buffer.

        // d2-curbuf: install the active buffer for this frame, then use the
        // ergonomic wrappers (`crd::draw::axis_triad(...)` etc.) instead of
        // the verbose `*_to(buffer, ...)` form. Both APIs coexist; the
        // canonical form is preferred for fan-out emission, the wrapper
        // form is preferred for one-line dev-console / editor calls.
        crd::draw::ScopedActiveBuffer scoped_buf{&m_draw_buffer};

        // World-axis triad at origin (3 arrows, RGB convention).
        crd::draw::axis_triad(crd::math::Mat4f::identity(), 1.0F);

        // (Floor grid is now drawn by the shader-based infinite grid pipeline,
        // wired through OverlayPassConfig::grid below. d2-grid superseded the
        // line-based grid call here.)

        // Box: wire + translucent solid fill at (2, 0, 0).
        crd::math::Mat4f box_world = crd::math::Mat4f::identity();
        box_world.c3.x = 2.0F;
        crd::draw::box_wire(box_world, {0.5F, 0.5F, 0.5F}, crd::draw::kBodyDynamic, 1.5F);
        crd::draw::box_solid(box_world, {0.5F, 0.5F, 0.5F}, crd::draw::Color{200, 200, 100, 80});

        // Sphere: wire + translucent solid at (-2, 0, 0). UV-everywhere = perfect alignment.
        crd::draw::sphere_wire({-2.0F, 0.0F, 0.0F}, 0.6F, crd::draw::kCyan);
        crd::draw::sphere_solid({-2.0F, 0.0F, 0.0F}, 0.6F, crd::draw::Color{0, 255, 255, 60});

        // Capsule: wire + translucent solid at (0, 0, 2).
        crd::draw::capsule_wire({0.0F, -0.4F, 2.0F}, {0.0F, 0.4F, 2.0F}, 0.4F,
                                crd::draw::kBodyKinematic);
        crd::draw::capsule_solid({0.0F, -0.4F, 2.0F}, {0.0F, 0.4F, 2.0F}, 0.4F,
                                 crd::draw::Color{80, 200, 240, 70});

        // Velocity-style arrow at (0, 1.5, 0) pointing +X.
        crd::draw::arrow({0.0F, 1.5F, 0.0F}, {1.0F, 0.0F, 0.0F}, 1.0F,
                         crd::draw::kVelocityArrow);

        // 3D cross marker (contact-point-style indicator).
        crd::draw::cross_3d({0.0F, 0.0F, -2.0F}, 0.3F, crd::draw::kContactPoint);

        // Joint-limit-style arc (90 degree sweep around Y axis).
        crd::draw::arc({0.0F, 0.5F, -2.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                       0.5F, 0.0F, 1.5707963F, crd::draw::kJointFrame);

        crd::renderer::FrameGraph draw_fg;
        const auto color_handle = draw_fg.import(&m_frp->color_image(),
                                                 crd::rhi::ImageAccess::ColorWrite);
        const auto depth_handle = draw_fg.import(&m_frp->depth_image(),
                                                 crd::rhi::ImageAccess::DepthRead);
        // d4: m_draw_cfg persists across frames so the ImGui control
        // panel can edit it (category mask, grid params, theme). Per-
        // frame fields are written here every frame; panel-driven fields
        // (grid colors / cell sizes / fade / category_mask) carry forward.
        m_draw_cfg.view_proj             = ctx.camera.projection * ctx.camera.view;
        m_draw_cfg.viewport_px           = {static_cast<crd::f32>(ext.width),
                                            static_cast<crd::f32>(ext.height)};
        m_draw_cfg.frame_in_flight_index = frame_index % 2;
        m_draw_cfg.grid.camera_pos       = ctx.camera_position;
        crd::draw::add_draw_overlay_pass(draw_fg, color_handle,
                                         depth_handle, m_draw_buffer, m_draw_cfg);
        if (draw_fg.build())
        {
            draw_fg.execute(m_device, cmd);
        }
    }

    if (m_show_wireframe && m_wf_pipeline && m_world)
    {
        const crd::rhi::RenderingColorAttachmentInfo wf_att{
            &m_frp->color_image(), crd::rhi::LoadOp::Load, crd::rhi::StoreOp::Store, {}};
        cmd.begin_rendering({ext, wf_att, nullptr});
        cmd.set_viewport(ext);
        cmd.set_scissor({0, 0, ext.width, ext.height});
        cmd.bind_pipeline(*m_wf_pipeline);

        const crd::math::Mat4f vp = ctx.camera.projection * ctx.camera.view;
        for (auto&& [entity, r] : m_world->query<crd::renderer::Renderable>()
                                       .skip_pending<crd::renderer::Renderable>())
        {
            (void)entity;
            if (r.vertex_buffer == nullptr) continue;
            cmd.push_constants(*m_wf_layout, crd::rhi::ShaderStage::Vertex, 0U,
                               static_cast<crd::u32>(sizeof(crd::math::Mat4f)), &vp);
            cmd.bind_vertex_buffer(*r.vertex_buffer, 0);
            if (r.index_buffer)
            {
                cmd.bind_index_buffer(*r.index_buffer, 0, r.index_type);
                cmd.draw_indexed(r.index_count, 0, 0);
            }
            else
                cmd.draw(r.vertex_count, 0);
        }
        cmd.end_rendering();
    }

    cmd.transition_image(m_frp->color_image(), crd::rhi::ImageAccess::ColorWrite,
                         crd::rhi::ImageAccess::TransferSrc);
    cmd.transition_image(sc_image, crd::rhi::ImageAccess::Undefined,
                         crd::rhi::ImageAccess::TransferDst);
    cmd.blit_image(m_frp->color_image(), sc_image, ext, m_swapchain.desc().extent);
    cmd.transition_image(sc_image, crd::rhi::ImageAccess::TransferDst,
                         crd::rhi::ImageAccess::ColorWrite);
}

void SandboxLayer::on_event(crd::app::Event& event)
{
    crd::app::EventDispatcher dispatcher(event);
    dispatcher.dispatch<crd::app::WindowResizeEvent>([this](crd::app::WindowResizeEvent& e)
    {
        if (e.width() <= 0 || e.height() <= 0) return false;
        m_device.wait_idle();
        const crd::rhi::Extent2D hint{static_cast<crd::u32>(e.width()),
                                      static_cast<crd::u32>(e.height())};
        m_swapchain.resize(hint);
        m_frp->resize(m_swapchain.desc().extent);
        return false;
    });
}

} // namespace crd::sandbox
