// sandbox/main.cpp — GEO-7 (D-007 row 72) on the RET-5 shell: the live Cerid window runs END TO END on the ONE
// graphics layer, and the scene is now REAL — the build-time cook (the GEO-6 incremental processor over
// assets/source's Khronos samples) mounts as a PACK, 10,000 instances of the cooked meshes instantiate into the
// ADR-0050 World (öbek-batch-shaped placement), and every frame the chunk-grain SceneRenderer extracts, culls
// (SpatialBVHIndex + frustum planes), partially re-uploads exactly the moved chunks, and draws ONE vertex-pulling
// instanced pass per mesh through gpu-context. The RET-6 grid/shape overlay composites over the scene WITH a real
// depth test now; ImGui + the profiler ride the present seam.
//
// CLI contract (the full-sweep smoke depends on it — preserved exactly):
//   --headless                    — exit after 1 frame; minimal boot smoke
//   --smoke-test [duration_secs]  — run the loop N seconds (default 3.0), FAIL (exit 2) if nothing presented.

#include <crd/anim/anim_resources.hpp>
#include <crd/app/app.hpp>
#include <crd/draw/overlay_pass.hpp>
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
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/openpbr_material.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/resources/texture_resource.hpp> // REN-2: the TXTR loader the material path needs
#include <crd/scene/render_components.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
#include <crd/scenerender/scene_renderer.hpp>
#include <crd/hesap/interp/keyframe.hpp>
#include <crd/time/rational_time.hpp>
#include <crd/timeline/timeline_eval.hpp>
#include <crd/timeline/timeline_resource.hpp>

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

// GEO-9: the CAMERA SHOT timeline — a 20 s looping automation pair (crane height + orbit radius, CubicHermite
// with zero tangents = eased dips) built as a real TimelineResource and sampled per frame in RATIONAL time.
// The wall clock quantizes onto a 24000 ticks/s grid at the query EDGE; keys live at 24 fps — the cross-rate
// exact segment selection is the GEO-9 evaluator doing its actual job, live.
[[nodiscard]] crd::timeline::TimelineResource build_camera_timeline(crd::memory::IAllocator* alloc)
{
    crd::timeline::TimelineResource tl(alloc);
    tl.name_off = tl.intern("sandbox-shot");

    const auto add_curve = [&](const char* target, crd::f32 a, crd::f32 b) {
        crd::timeline::AutomationRec rec;
        rec.target_off = tl.intern(target);
        rec.rate       = crd::time::kRate24;
        rec.interp     = static_cast<crd::u8>(crd::hesap::interp::KeyInterp::CubicHermite);
        rec.key_count  = 3;
        rec.ticks_off  = static_cast<crd::u32>(tl.auto_ticks.size());
        rec.values_off = static_cast<crd::u32>(tl.auto_values.size());
        const crd::i64 ticks[3] = {0, 240, 480}; // 0 s · 10 s · 20 s at 24 fps
        const crd::f32 vals[3]  = {a, b, a};     // out and back — the loop boundary is C1-continuous
        for (crd::i64 t : ticks) { tl.auto_ticks.push_back(t); }
        for (crd::f32 v : vals) // [in_tangent · value · out_tangent] triples, zero tangents = ease in/out
        {
            tl.auto_values.push_back(0.0F);
            tl.auto_values.push_back(v);
            tl.auto_values.push_back(0.0F);
        }
        tl.automation.push_back(rec);
    };
    add_curve("camera.height", 32.0F, 7.0F); // crane down for the close pass over the fox ring
    add_curve("camera.radius", 55.0F, 26.0F);
    return tl;
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

// every MESH / SKEL / ANIM artifact id in the mounted pack (read straight from the PACK manifest), with each
// mesh's manifest DEBUG NAME (the source rel path — the showcase sorts formats by it)
void collect_pack_artifacts(const crd::platform::fs::Path& pack_path, crd::memory::IAllocator* alloc,
                            crd::containers::Array<crd::resources::ResourceId>& out_meshes,
                            crd::containers::Array<crd::containers::String>&    out_mesh_names,
                            crd::containers::Array<crd::resources::ResourceId>& out_skeletons,
                            crd::containers::Array<crd::resources::ResourceId>& out_clips)
{
    crd::containers::Array<crd::u8> bytes(alloc);
    if (!crd::platform::fs::read_file_binary(pack_path, bytes)) { return; }
    crd::resources::CrdrFile file(alloc);
    if (crd::resources::crdr_read(crd::containers::as_const_span(bytes), file, alloc)
        != crd::resources::CrdrError::Ok)
    {
        return;
    }
    const crd::resources::CrdrChunk* mfst = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_MFST);
    const crd::resources::CrdrChunk* strp = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_STRP);
    if (mfst == nullptr) { return; }
    crd::containers::Array<crd::resources::ManifestEntry> entries(alloc);
    if (!crd::resources::manifest_read_entries(mfst->payload, entries, alloc)) { return; }
    for (const auto& e : entries)
    {
        if (e.type_fourcc == crd::resources::kFourCC_MESH)
        {
            out_meshes.push_back(e.id);
            crd::containers::String name(alloc);
            if (strp != nullptr && e.name_strp_idx < strp->payload.size())
            {
                name.append(reinterpret_cast<const char*>(strp->payload.data()) + e.name_strp_idx);
            }
            out_mesh_names.push_back(static_cast<crd::containers::String&&>(name));
        }
        if (e.type_fourcc == crd::anim::kFourCC_SKEL) { out_skeletons.push_back(e.id); }
        if (e.type_fourcc == crd::anim::kFourCC_ANIM) { out_clips.push_back(e.id); }
    }
}

[[nodiscard]] bool name_contains(const crd::containers::String& name, const char* needle)
{
    return std::strstr(name.c_str(), needle) != nullptr;
}

// world-AABB extractor for the spatial index: Transform (the watched trigger) + a fixed generous half-extent
// (instances are normalized to ~unit size at spawn)
struct SceneExtractor final : crd::scene::IAabbExtractor
{
    [[nodiscard]] crd::geometry::primitives::AABB3<crd::f32>
    extract(crd::scene::EntityId, crd::scene::ComponentId, const void* data) const override
    {
        const auto*            t = static_cast<const crd::scene::Transform*>(data);
        const crd::math::Vec3f p = crd::math::to_raw_vec(t->translation);
        constexpr crd::f32     h = 1.5F;
        return {{p.x - h, p.y - h, p.z - h}, {p.x + h, p.y + h, p.z + h}};
    }
};

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered so log lines survive a crash

    bool     headless         = false;
    bool     smoke_test       = false;
    crd::f64 smoke_duration_s = 3.0;
    // Fifo (vsync) is the right DEFAULT — it is what a shipped app wants, and an uncapped loop burns the GPU for
    // frames nobody sees. But it also makes the frame rate a property of the DISPLAY, not of the renderer, so a
    // vsynced number can never answer "how fast is the renderer?". `--present immediate` removes the cap so the
    // real cost is measurable; that is the only honest way to profile the frame.
    crd::gpu::PresentMode present_mode   = crd::gpu::PresentMode::Fifo;
    bool                  force_readback = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--headless") == 0) { headless = true; }
        // REN-8: A/B the per-frame readback copy. Run-to-run fps varies by ~10 on this host, so a claim like
        // "removing the readback made it faster" is only honest if BOTH arms are measured on the same build.
        else if (std::strcmp(argv[i], "--readback") == 0) { force_readback = true; }
        else if (std::strcmp(argv[i], "--present") == 0 && i + 1 < argc)
        {
            ++i;
            if (std::strcmp(argv[i], "immediate") == 0)    { present_mode = crd::gpu::PresentMode::Immediate; }
            else if (std::strcmp(argv[i], "mailbox") == 0) { present_mode = crd::gpu::PresentMode::Mailbox; }
            else                                           { present_mode = crd::gpu::PresentMode::Fifo; }
        }
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
    app_desc.window.title = crd::containers::String("Cerid Sandbox — 10k instances on gpu-context (GEO-7)");
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
    gpu_cfg.enable_validation = !headless;
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
    auto     surface = raster->create_present_surface(native_window_of(app), win_w, win_h, present_mode);
    if (surface == nullptr)
    {
        CRD_LOG_ERROR(g_log_sandbox, "Present surface creation failed");
        crd::log::shutdown();
        return 1;
    }

    // ── the GEO-7 scene: the build-time-cooked PACK → World → SceneRenderer ─────────────────────────────────────
    crd::memory::TlsfAllocator scene_alloc(256U << 20U);

    // GEO-9: the camera-shot timeline (automation-driven crane move, sampled per frame in rational time)
    const crd::timeline::TimelineResource camera_timeline = build_camera_timeline(&scene_alloc);

    crd::resources::ResourceManager rm(&scene_alloc);
    crd::resources::register_mesh_loader(&rm, nullptr);
    crd::resources::register_openpbr_material_loader(&rm, nullptr);
    // ⛔ REN-2: WITHOUT this the material loader resolves `textures.base_color` to a TXTR id that nothing can
    // load — every material silently falls back to its flat colour and the sandbox shows NO sampled albedo at
    // all, while the engine capability and its gate are both green. The engine having a feature and the app
    // reaching it are two different facts; only the sandbox proves the second.
    crd::resources::register_texture_loader(&rm, nullptr);
    crd::anim::register_anim_loaders(&rm, nullptr); // GEO-8: SKEL + ANIM
    const crd::platform::fs::Path pack_path = crd::platform::fs::executable_dir()
                                              / crd::containers::StringView(CRD_DEMO_ASSETS_REL_PACK);
    crd::containers::Array<crd::resources::ResourceId> pack_meshes(&scene_alloc);
    crd::containers::Array<crd::containers::String>    pack_mesh_names(&scene_alloc);
    crd::containers::Array<crd::resources::ResourceId> pack_skeletons(&scene_alloc);
    crd::containers::Array<crd::resources::ResourceId> pack_clips(&scene_alloc);
    if (rm.mount_manifest(pack_path.generic()).is_valid())
    {
        collect_pack_artifacts(pack_path, &scene_alloc, pack_meshes, pack_mesh_names, pack_skeletons, pack_clips);
    }
    CRD_LOG_INFO(g_log_sandbox, "Demo pack: {} mesh / {} skeleton / {} clip artifact(s)", pack_meshes.size(),
                 pack_skeletons.size(), pack_clips.size());

    crd::scene::World world{&scene_alloc};
    world.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait(),
                                                    crd::scene::SpatialBVH{});
    crd::scene::register_render_components(world);
    auto*          bvh = world.find_index<crd::scene::SpatialBVHIndex>();
    SceneExtractor extractor;
    if (bvh != nullptr)
    {
        bvh->configure(&extractor,
                       crd::geometry::spatial::OctreeBuildOptions<crd::f32>{
                           crd::geometry::primitives::AABB3<crd::f32>{{-140, -30, -140}, {140, 30, 140}}, 2.0F, 16U,
                           10U});
    }

    // 10,000 instances on a 100×100 grid, meshes cycled, each normalized to ~1.5 units from its cooked bounds,
    // each carrying its own AUTHORED material (the PRIM chunk's material id — GEO-3 stage 4's wiring, live)
    constexpr crd::u32 side = 100U;
    struct Cell
    {
        crd::scene::EntityId entity;
        crd::f32             x, z, scale;
    };
    crd::containers::Array<Cell> cells(&scene_alloc);
    // split the pack's meshes three ways: SKINNED → the animated ring; STL/OBJ/PLY/3MF imports → the GEO
    // MONUMENTS (the multi-format showcase); the rest (glTF) → the 10k grid
    crd::containers::Array<crd::resources::ResourceId> static_meshes(&scene_alloc);
    crd::containers::Array<crd::resources::ResourceId> skinned_meshes(&scene_alloc);
    crd::containers::Array<crd::resources::ResourceId> monument_meshes(&scene_alloc);
    for (crd::usize mi = 0; mi < pack_meshes.size(); ++mi)
    {
        auto handle = rm.load_sync<crd::resources::MeshResource>(pack_meshes[mi]);
        if (handle.state() != crd::resources::LoadState::Ready || handle.get() == nullptr) { continue; }
        if (handle.get()->has_skin()) { skinned_meshes.push_back(pack_meshes[mi]); }
        else if (name_contains(pack_mesh_names[mi], ".stl") || name_contains(pack_mesh_names[mi], ".obj")
                 || name_contains(pack_mesh_names[mi], ".ply") || name_contains(pack_mesh_names[mi], ".3mf"))
        {
            monument_meshes.push_back(pack_meshes[mi]);
        }
        else { static_meshes.push_back(pack_meshes[mi]); }
    }
    if (pack_meshes.size() > 0U && static_meshes.size() > 0U)
    {
        for (crd::u32 gz = 0; gz < side; ++gz)
        {
            for (crd::u32 gx = 0; gx < side; ++gx)
            {
                const crd::resources::ResourceId mesh_id =
                    static_meshes[(static_cast<crd::usize>(gz) * side + gx) % static_meshes.size()];
                auto handle = rm.load_sync<crd::resources::MeshResource>(mesh_id);
                if (handle.state() != crd::resources::LoadState::Ready || handle.get() == nullptr) { continue; }
                const auto* mesh = handle.get();

                crd::f32 scale = 1.0F;
                if (mesh->has_bounds())
                {
                    const crd::f32 ex = mesh->bounds_max[0] - mesh->bounds_min[0];
                    const crd::f32 ey = mesh->bounds_max[1] - mesh->bounds_min[1];
                    const crd::f32 ez = mesh->bounds_max[2] - mesh->bounds_min[2];
                    crd::f32       m  = ex > ey ? ex : ey;
                    m                 = m > ez ? m : ez;
                    if (m > 1.0e-6F) { scale = 1.5F / m; }
                }
                crd::resources::ResourceId material{};
                if (mesh->primitives.size() > 0U) { material = mesh->primitives[0].material_id; }

                const crd::f32 x = (static_cast<crd::f32>(gx) - 49.5F) * 2.0F;
                const crd::f32 z = (static_cast<crd::f32>(gz) - 49.5F) * 2.0F;

                const crd::scene::EntityId e = world.spawn();
                crd::scene::Transform      t;
                t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{x, 0.0F, z});
                t.scale       = {scale, scale, scale};
                t.world = crd::math::from_trs(crd::math::Vec3f{x, 0.0F, z}, crd::math::Quatf::identity(), t.scale);
                world.add_component(e, t);
                world.add_component(e, crd::scene::MeshRenderer{mesh_id, material});
                cells.push_back(Cell{e, x, z, scale});
            }
        }
    }
    // GEO-8: the animated ring — skinned characters (the Fox) circle the origin, clips cycled, phases staggered
    crd::containers::Array<crd::scene::EntityId> animated(&scene_alloc);
    if (skinned_meshes.size() > 0U && pack_skeletons.size() > 0U)
    {
        constexpr crd::u32 ring_count = 24U;
        auto handle = rm.load_sync<crd::resources::MeshResource>(skinned_meshes[0]);
        const auto* mesh = handle.get();
        crd::f32    scale = 1.0F;
        if (mesh != nullptr && mesh->has_bounds())
        {
            const crd::f32 ex = mesh->bounds_max[0] - mesh->bounds_min[0];
            const crd::f32 ey = mesh->bounds_max[1] - mesh->bounds_min[1];
            const crd::f32 ez = mesh->bounds_max[2] - mesh->bounds_min[2];
            crd::f32       mx = ex > ey ? ex : ey;
            mx                = mx > ez ? mx : ez;
            if (mx > 1.0e-6F) { scale = 2.5F / mx; }
        }
        crd::resources::ResourceId material{};
        if (mesh != nullptr && mesh->primitives.size() > 0U) { material = mesh->primitives[0].material_id; }
        for (crd::u32 i = 0; i < ring_count; ++i)
        {
            const crd::f32 ang = static_cast<crd::f32>(i) * (6.2831853F / static_cast<crd::f32>(ring_count));
            const crd::f32 x   = crd::math::cos(ang) * 9.0F;
            const crd::f32 z   = crd::math::sin(ang) * 9.0F;
            const crd::scene::EntityId e = world.spawn();
            crd::scene::Transform      t;
            t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{x, 0.0F, z});
            t.rotation    = crd::math::from_axis_angle(crd::math::Vec3f{0, 1, 0}, -ang);
            t.scale       = {scale, scale, scale};
            t.world       = crd::math::from_trs(crd::math::Vec3f{x, 0.0F, z}, t.rotation, t.scale);
            world.add_component(e, t);
            world.add_component(e, crd::scene::MeshRenderer{skinned_meshes[0], material});
            crd::scene::SkeletonAnimator animator;
            animator.skeleton = pack_skeletons[0];
            if (pack_clips.size() > 0U) { animator.clip = pack_clips[i % pack_clips.size()]; }
            animator.time = static_cast<crd::f32>(i) * 0.17F; // staggered phases
            world.add_component(e, animator);
            animated.push_back(e);
        }
    }
    // the GEO MONUMENTS: every non-glTF import (STL icosahedron · OBJ torus with GENERATED normals · the teal
    // 3MF box whose displaycolor travelled sRGB→linear→PBRM→PRIM→this frame) — large, slowly spinning
    struct Monument
    {
        crd::scene::EntityId entity;
        crd::f32             x, z, scale;
    };
    crd::containers::Array<Monument> monuments(&scene_alloc);
    for (crd::usize mi = 0; mi < monument_meshes.size(); ++mi)
    {
        auto handle = rm.load_sync<crd::resources::MeshResource>(monument_meshes[mi]);
        const auto* mesh = handle.get();
        if (mesh == nullptr) { continue; }
        crd::f32 scale = 1.0F;
        if (mesh->has_bounds())
        {
            const crd::f32 ex = mesh->bounds_max[0] - mesh->bounds_min[0];
            const crd::f32 ey = mesh->bounds_max[1] - mesh->bounds_min[1];
            const crd::f32 ez = mesh->bounds_max[2] - mesh->bounds_min[2];
            crd::f32       mx = ex > ey ? ex : ey;
            mx                = mx > ez ? mx : ez;
            if (mx > 1.0e-6F) { scale = 5.0F / mx; }
        }
        crd::resources::ResourceId material{};
        if (mesh->primitives.size() > 0U) { material = mesh->primitives[0].material_id; }
        const crd::f32 ang = static_cast<crd::f32>(mi) * (6.2831853F / static_cast<crd::f32>(monument_meshes.size()))
                             + 0.5F;
        const crd::f32 x = crd::math::cos(ang) * 17.0F;
        const crd::f32 z = crd::math::sin(ang) * 17.0F;
        const crd::scene::EntityId e = world.spawn();
        crd::scene::Transform      t;
        t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{x, 3.0F, z});
        t.scale       = {scale, scale, scale};
        t.world       = crd::math::from_trs(crd::math::Vec3f{x, 3.0F, z}, crd::math::Quatf::identity(), t.scale);
        world.add_component(e, t);
        world.add_component(e, crd::scene::MeshRenderer{monument_meshes[mi], material});
        monuments.push_back(Monument{e, x, z, scale});
    }
    CRD_LOG_INFO(g_log_sandbox, "Scene: {} static + {} animated + {} monuments", cells.size(), animated.size(),
                 monuments.size());

    auto scene_renderer_ptr = std::make_unique<crd::scenerender::SceneRenderer>(&scene_alloc);
    auto& scene_renderer    = *scene_renderer_ptr;
    const bool scene_ready = scene_renderer.init(*raster, rm) && scene_renderer.init_programs(*vk)
                             && cells.size() > 0U;
    if (!scene_ready) { CRD_LOG_WARN(g_log_sandbox, "Scene renderer unavailable — falling back to overlay-only"); }

    // the canvas now carries DEPTH — the scene pass writes it; the overlay depth-tests against it
    auto canvas = raster->create_color_depth_target(surface->width(), surface->height());
    if (canvas == nullptr)
    {
        CRD_LOG_ERROR(g_log_sandbox, "Canvas creation failed");
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

    // RET-6 pt 4: the debug-draw overlay (axis triad + the infinite grid) composes over the scene
    const bool draw_ready = crd::draw::init(*vk, *raster);
    if (!draw_ready) { CRD_LOG_WARN(g_log_sandbox, "crd-draw init failed -- continuing without the draw overlay"); }
    crd::draw::RenderBuffer draw_buf(crd::memory::default_allocator());
    if (draw_ready) // the RET-6 debug-draw suite over the real scene depth (wire shapes + the translucent slab)
    {
        crd::draw::axis_triad_to(draw_buf, crd::math::Mat4f::identity(), 2.0F, 3.0F);
        crd::draw::sphere_wire_to(draw_buf, {13.0F, 1.5F, 0.0F}, 1.5F, crd::draw::kCyan);
        crd::math::Mat4f box_world = crd::math::Mat4f::identity();
        box_world.c3               = {-13.0F, 1.0F, 0.5F, 1.0F};
        crd::draw::box_wire_to(draw_buf, box_world, {1.0F, 1.0F, 1.0F}, crd::draw::kOrange, 2.0F);
        crd::draw::capsule_wire_to(draw_buf, {0.0F, 0.7F, -13.0F}, {0.0F, 2.6F, -13.0F}, 0.7F, crd::draw::kMagenta);
        crd::math::Mat4f slab_world = crd::math::Mat4f::identity();
        slab_world.c3               = {0.0F, 0.35F, 13.0F, 1.0F};
        crd::draw::box_solid_to(draw_buf, slab_world, {1.4F, 0.35F, 0.85F},
                                crd::draw::Color{255, 200, 40, 90}); // translucent amber — alpha blending live
    }

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
    crd::scenerender::SyncStats   last_sync{};
    // REN-8: the sandbox PRESENTS, it never reads pixels back — so skip the per-frame full-target host copy the
    // frame graph does for `read_pixel`. Measured cost of leaving it on: a 7.1 ms stall behind 1.8 ms of passes.
    scene_renderer.set_readback_enabled(force_readback);

    // ⛔ HARD RULE: the grid goes through OUR frame graph. `record_overlay_pass` runs as a pass of the scene's
    // graph, so `submit_overlay`'s `draw_overlay` calls hit the raster context's RECORDING path and land in the
    // frame's one command buffer instead of each doing its own submit+wait.
    struct OverlayCtx
    {
        crd::draw::RenderBuffer*     buf    = nullptr;
        crd::gpu::IRasterTarget*     target = nullptr;
        crd::draw::OverlayPassConfig cfg{};
    } overlay_ctx{&draw_buf, canvas.get(), {}};
    scene_renderer.set_overlay_pass(
        [](crd::gpu::IFrameContext& ctx, void* user) {
            auto* o = static_cast<OverlayCtx*>(user);
            if (!crd::draw::submit_overlay(*o->target, *o->buf, o->cfg))
            {
                CRD_LOG_WARN(g_log_sandbox, "draw overlay submission refused");
            }
            (void)ctx; // the target is the same imported canvas; ctx.raster() is already in recording mode
        },
        &overlay_ctx);

    crd::scenerender::RenderStats last_draw{};
    // REN-8: the frame's phase breakdown. `render` already reports its own gpu/cpu split; these cover the REST
    // of the loop, which the first attribution showed to be ~64% of the frame and entirely unmeasured. Timing
    // the whole loop and not just the renderer is the difference between "the GPU is fast" and "the frame is
    // fast" — they turned out to be very different statements here.
    struct PhaseMs
    {
        double sync = 0.0, render = 0.0, overlay = 0.0, imgui = 0.0, present = 0.0, total = 0.0;
    } phase;
    // ⛔ Report the MEAN over the run, not the last frame. A single trailing sample is noise — the first run of
    // this instrumentation showed `sync` at 3.3 ms and 9.9 ms on two runs of the same build purely by which
    // frame happened to be last, which is exactly the kind of number that sends optimization work in the wrong
    // direction. Averages get compared; a lone sample gets believed.
    PhaseMs  phase_sum;
    crd::u64 phase_frames = 0;
    const auto now_ms = [] { return std::chrono::steady_clock::now(); };
    const auto ms_between = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto smoke_start_time    = std::chrono::steady_clock::now();
    while (app.is_running())
    {
        if (!app.tick()) { break; }

        const auto     cur   = app.window().framebuffer_size();
        const crd::u32 cur_w = cur.width > 0 ? static_cast<crd::u32>(cur.width) : 0U;
        const crd::u32 cur_h = cur.height > 0 ? static_cast<crd::u32>(cur.height) : 0U;
        if ((cur_w != win_w || cur_h != win_h) && cur_w > 0U && cur_h > 0U)
        {
            win_w = cur_w;
            win_h = cur_h;
            if (surface->resize(win_w, win_h))
            {
                canvas = raster->create_color_depth_target(surface->width(), surface->height());
                if (canvas == nullptr) { break; }
            }
        }

        const crd::f64 tsec =
            std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - smoke_start_time).count();

        // GEO-8: advance the animated ring's playheads (declared writes — palettes re-sample each sync)
        {
            static crd::f64 s_last = 0.0;
            const crd::f32  dt     = static_cast<crd::f32>(tsec - s_last);
            s_last                 = tsec;
            for (const crd::scene::EntityId e : animated)
            {
                if (auto* a = world.get_component_mut<crd::scene::SkeletonAnimator>(e); a != nullptr)
                {
                    a->time += dt * a->speed;
                }
            }
        }

        // the monuments spin slowly (transform upserts → live BVH refresh + chunk-grain partial re-upload)
        for (const auto& mon : monuments)
        {
            crd::scene::Transform t;
            t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(
                crd::math::Vec3f{mon.x, 3.0F, mon.z});
            t.rotation = crd::math::from_axis_angle(crd::math::Vec3f{0, 1, 0}, static_cast<crd::f32>(tsec) * 0.6F);
            t.scale    = {mon.scale, mon.scale, mon.scale};
            t.world    = crd::math::from_trs(crd::math::Vec3f{mon.x, 3.0F, mon.z}, t.rotation, t.scale);
            world.add_component(mon.entity, t);
        }

        // a travelling wave: ONE grid row per frame bobs (the chunk-grain partial re-upload, live every frame)
        if (scene_ready && cells.size() == side * side)
        {
            const crd::u32 row = frame % side;
            for (crd::u32 gx = 0; gx < side; ++gx)
            {
                Cell&          cell = cells[static_cast<crd::usize>(row) * side + gx];
                const crd::f32 y =
                    1.2F * crd::math::sin(static_cast<crd::f32>(tsec) * 2.0F + cell.x * 0.35F + cell.z * 0.21F);
                crd::scene::Transform t;
                t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(
                    crd::math::Vec3f{cell.x, y, cell.z});
                t.scale = {cell.scale, cell.scale, cell.scale};
                t.world = crd::math::from_trs(crd::math::Vec3f{cell.x, y, cell.z}, crd::math::Quatf::identity(),
                                              t.scale);
                world.add_component(cell.entity, t);
            }
        }

        // the orbit camera over the field — height + radius DRIVEN BY THE GEO-9 TIMELINE (a real
        // TimelineResource sampled in rational time; the 20 s shot loops)
        const crd::i64 shot_ticks =
            static_cast<crd::i64>(tsec * 24000.0) % (20LL * 24000LL); // wall clock → the fine rational grid
        const crd::time::RationalTime shot_t{shot_ticks, crd::time::make_rate(24000, 1)};
        crd::f32                      cam_height = 32.0F;
        crd::f32                      cam_radius = 55.0F;
        (void)crd::timeline::automation_value(camera_timeline, 0, shot_t, cam_height);
        (void)crd::timeline::automation_value(camera_timeline, 1, shot_t, cam_radius);
        const float            orbit = static_cast<float>(tsec * 0.12);
        const crd::math::Vec3f eye{crd::math::sin(orbit) * cam_radius, cam_height,
                                   crd::math::cos(orbit) * cam_radius};
        const crd::math::Mat4f view =
            crd::math::look_at(eye, crd::math::Vec3f{0.0F, 0.0F, 0.0F}, crd::math::Vec3f{0.0F, 1.0F, 0.0F});
        const float aspect =
            static_cast<float>(surface->width()) / static_cast<float>(surface->height() > 0U ? surface->height() : 1U);
        const crd::math::Mat4f proj = crd::math::perspective_reverse_z(1.0472F, aspect, 0.1F);
        const crd::math::Mat4f vp   = proj * view;

        const auto t_frame_begin = now_ms();
        if (scene_ready)
        {
            last_sync         = scene_renderer.sync(world);
            const auto t_sync = now_ms();
            phase.sync        = ms_between(t_frame_begin, t_sync);
            last_draw = scene_renderer.render(*canvas, vp, crd::math::Vec3f{0.35F, 1.0F, 0.25F},
                                              crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F}, bvh);
            phase.render = ms_between(t_sync, now_ms());
            if (last_draw.draws == 0U) // everything culled (or nothing loadable): still present a cleared frame
            {
                raster->clear(*canvas, crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F});
            }
        }
        else { raster->clear(*canvas, crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F}); }

        // ⛔ HARD RULE: the infinite grid is a RENDER PASS, so it runs INSIDE the scene's frame graph (registered
        // via set_overlay_pass, recorded into the same command buffer, one submission). Its config is refreshed
        // here each frame; the recording itself happens when the graph executes the "overlay" pass above.
        if (draw_ready)
        {
            overlay_ctx.cfg.view_proj   = vp;
            overlay_ctx.cfg.viewport_px = {static_cast<crd::f32>(surface->width()),
                                           static_cast<crd::f32>(surface->height())};
            overlay_ctx.cfg.time_s      = static_cast<crd::f32>(tsec);
            overlay_ctx.cfg.depth_test  = crd::gpu::DepthCompare::GreaterEqual; // the canvas HAS depth: grid occludes
            overlay_ctx.cfg.grid.enabled    = true;
            overlay_ctx.cfg.grid.camera_pos = eye;
            overlay_ctx.cfg.grid.apply_theme();
        }
        const auto t_after_scene = now_ms();

        const auto t_after_overlay = now_ms();
        phase.overlay              = ms_between(t_after_scene, t_after_overlay);

        imgui_backend->new_frame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        {
            ImGui::Begin("Cerid Sandbox — GEO-7");
            ImGui::Text("frame %u  |  %ux%u", frame, surface->width(), surface->height());
            ImGui::Text("instances: %u drawn / %u culled / %u total", last_draw.drawn_instances,
                        last_draw.culled_instances, last_sync.total_instances);
            ImGui::Text("draws: %u (one per mesh group; %u groups)", last_draw.draws, last_sync.groups);
            // REN-8: the frame's honest attribution. `gpu` is what the DEVICE spent (frame-graph timestamps);
            // `cpu` is the wall-clock of the whole render call including the fence wait. A large `stall` means
            // the frame is dominated by waiting, not by rendering — which is what the ~12 ms/frame turned out
            // to be, and is why REN-8's async-across-frames + direct present is the fix rather than shader work.
            ImGui::Text("gpu: %.3f ms (%u passes) | cpu: %.3f ms | stall: %.3f ms", last_draw.gpu_ms,
                        last_draw.timed_passes, last_draw.cpu_ms,
                        last_draw.cpu_ms > last_draw.gpu_ms ? last_draw.cpu_ms - last_draw.gpu_ms : 0.0);
            ImGui::Text("phases: sync %.2f | render %.2f | overlay %.2f | imgui %.2f | present %.2f = %.2f ms",
                        phase.sync, phase.render, phase.overlay, phase.imgui, phase.present, phase.total);
            ImGui::Text("shot: %.2fs / 20s  height %.1f  radius %.1f (GEO-9 timeline automation)",
                        static_cast<double>(shot_ticks) / 24000.0, static_cast<double>(cam_height),
                        static_cast<double>(cam_radius));
            ImGui::Text("sync upload: %llu B (%u dirty chunk runs)%s",
                        static_cast<unsigned long long>(last_sync.uploaded_bytes), last_sync.dirty_runs,
                        last_sync.structural_rebuild ? " [rebuild]" : "");
            ImGui::TextUnformatted("import -> cook (GEO-6) -> mount -> instantiate -> chunk-grain cull+draw");
            ImGui::End();
            profiler_panel.draw();
        }
        ImGui::Render();
        const auto t_after_imgui = now_ms();
        phase.imgui              = ms_between(t_after_overlay, t_after_imgui);

        if (surface->present(*canvas, &crd::imgui::ImGuiGpuBackend::overlay_thunk, imgui_backend.get()))
        {
            ++frames_with_present;
        }
        phase.present = ms_between(t_after_imgui, now_ms());
        phase.total   = ms_between(t_frame_begin, now_ms());
        phase_sum.sync += phase.sync;
        phase_sum.render += phase.render;
        phase_sum.overlay += phase.overlay;
        phase_sum.imgui += phase.imgui;
        phase_sum.present += phase.present;
        phase_sum.total += phase.total;
        ++phase_frames;

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
                CRD_LOG_INFO(g_log_sandbox,
                             "Smoke-test: PASS — {} frames presented over {:.2f}s ({:.1f} fps avg); "
                             "{} instances drawn last frame",
                             frames_with_present, elapsed,
                             static_cast<crd::f64>(frames_with_present) / elapsed, last_draw.drawn_instances);
        // REN-8: the smoke run prints the SAME attribution, so a headless/CI run reports where the frame went
        // rather than only whether it presented.
        CRD_LOG_INFO(g_log_sandbox, "REN-8 frame split: gpu {:.3f} ms ({} passes) | cpu {:.3f} ms | stall {:.3f} ms",
                     last_draw.gpu_ms, last_draw.timed_passes, last_draw.cpu_ms,
                     last_draw.cpu_ms > last_draw.gpu_ms ? last_draw.cpu_ms - last_draw.gpu_ms : 0.0);
        const double pf = phase_frames > 0 ? static_cast<double>(phase_frames) : 1.0;
        CRD_LOG_INFO(g_log_sandbox,
                     "REN-8 phases (MEAN over {} frames): sync {:.3f} | render {:.3f} | overlay {:.3f} | "
                     "imgui {:.3f} | present {:.3f} | loop total {:.3f} ms",
                     phase_frames, phase_sum.sync / pf, phase_sum.render / pf, phase_sum.overlay / pf,
                     phase_sum.imgui / pf, phase_sum.present / pf, phase_sum.total / pf);
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
    crd::draw::shutdown();
    imgui_backend.reset();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    scene_renderer_ptr.reset(); // the scene's GPU buffers/programs die BEFORE the raster context and device
    surface.reset();
    canvas.reset();
    raster.reset();
    gpu_context.reset();

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
