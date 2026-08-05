// sandbox/gizmo_probe.cpp — REN-39: THE GIZMO, CLOSE UP.
//
// A second, deliberately SMALL sandbox whose only subject is the debug-draw axis triad: same engine path as the
// big sandbox (windowed VulkanGpuContext → IRasterContext → present surface → SceneRenderer hosting an AUTHORED
// frame graph, with the overlay woven in as a pass), but with the camera parked a few metres from the origin so
// the triad fills the frame and every pixel of it is inspectable.
//
// ⛔ IT USES OUR OWN ASSETS, exactly like the sandbox: `CRD_ASSETS_DIR` installs the asset root BEFORE
// `init_programs` (so materials / lighting / vertex programs / post graphs all resolve disk-first), and the frame
// is chosen BY NAME from `assets/frame/*.frame.toml` — the same authored CSM + AgX graph and the same authored
// techniques the sandbox runs. Nothing here hand-builds a pass.
//
// Why it exists: the "dotted ghost beside the up vector" is a few pixels wide in the big sandbox, buried in 10k
// instances. Here it is the whole screen, one primitive at a time.
//
// CLI:
//   --shapes <mode>      triad (default) | stem | arrow | all — WHICH debug primitives to submit
//   --radius <r>         camera distance from the origin (default 7)
//   --height <h>         camera height (default 2.6)
//   --freeze             stop the orbit (a static frame — a stale-matrix ghost lands ON the line, not beside it)
//   --no-grid            drop the infinite grid
//   --no-backdrop        no backdrop geometry (the graph still needs one draw — see `kBackdropMin`)
//   --frame <name>       frame asset (default frame/forward_csm_agx.frame.toml)
//   --screenshot <path>  dump the presented canvas to a 24-bit BMP at `--screenshot-at` and exit
//   --screenshot-at <s>  when to dump (default 2.5 s)
//   --smoke-test [s]     run N seconds then exit (FAIL if nothing presented)
//   --present <mode>     fifo | immediate | mailbox   ·   --no-validation

#include <crd/app/app.hpp>
#include <crd/draw/overlay_pass.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/gpu/context.hpp>
#ifdef _WIN32                              // REN-39-D1: `--backend dx12` — the D3D12 backend is Windows-only
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#endif
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/log/log.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/openpbr_material.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/resources/texture_resource.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
#include <crd/scenerender/scene_renderer.hpp>

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

CRD_DEFINE_LOG_CHANNEL(g_log_gizmo, "GizmoProbe", crd::log::LogLevel::Trace)

namespace
{
// ⛔ The authored frame graph's geometry pass must have at least ONE draw: `SceneRenderer::render` returns early
// on an empty draw list, and the overlay is a pass INSIDE that graph — no draw, no gizmo. So even `--no-backdrop`
// keeps one instance (parked far behind the camera's subject).
constexpr crd::u32 kBackdropMin = 1U;

[[nodiscard]] void* native_window_of(crd::app::Application& app)
{
#ifdef _WIN32
    return glfwGetWin32Window(static_cast<GLFWwindow*>(app.window().native_handle()));
#else
    (void)app;
    return nullptr;
#endif
}

[[nodiscard]] inline const char* probe_getenv(const char* name) noexcept
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

// the mesh ids in the mounted demo pack (manifest-read, same as the sandbox's collector minus the skel/anim half)
void collect_pack_meshes(const crd::platform::fs::Path& pack_path, crd::memory::IAllocator* alloc,
                         crd::containers::Array<crd::resources::ResourceId>& out)
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
    if (mfst == nullptr) { return; }
    crd::containers::Array<crd::resources::ManifestEntry> entries(alloc);
    if (!crd::resources::manifest_read_entries(mfst->payload, entries, alloc)) { return; }
    for (const auto& e : entries)
    {
        if (e.type_fourcc == crd::resources::kFourCC_MESH) { out.push_back(e.id); }
    }
}

// 24-bit BMP of the presented canvas — the same dump the sandbox's `--screenshot` writes.
void write_bmp(const char* path, crd::gpu::IRasterTarget& target)
{
    const crd::u32 w   = target.width();
    const crd::u32 h   = target.height();
    const crd::u32 row = ((w * 3U + 3U) / 4U) * 4U;
    crd::containers::Array<unsigned char> bmp(crd::memory::default_allocator());
    bmp.resize(54U + static_cast<crd::usize>(row) * h, static_cast<unsigned char>(0));
    const auto p4 = [&](crd::u32 o, crd::u32 v) {
        for (crd::u32 k = 0; k < 4U; ++k) { bmp[o + k] = static_cast<unsigned char>((v >> (8U * k)) & 0xFFU); }
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    p4(2U, 54U + row * h);
    p4(10U, 54U);
    p4(14U, 40U);
    p4(18U, w);
    p4(22U, h);
    bmp[26] = 1U;
    bmp[28] = 24U;
    p4(34U, row * h);
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x)
        {
            const crd::u32   px = target.read_pixel(x, y); // 0xAABBGGRR
            const crd::usize o =
                54U + static_cast<crd::usize>(h - 1U - y) * row + static_cast<crd::usize>(x) * 3U;
            bmp[o]      = static_cast<unsigned char>((px >> 16U) & 0xFFU); // B
            bmp[o + 1U] = static_cast<unsigned char>((px >> 8U) & 0xFFU);  // G
            bmp[o + 2U] = static_cast<unsigned char>(px & 0xFFU);          // R
        }
    }
    std::FILE* f = nullptr;
#ifdef _WIN32
    (void)fopen_s(&f, path, "wb");
#else
    f = std::fopen(path, "wb");
#endif
    if (f != nullptr)
    {
        (void)std::fwrite(bmp.data(), 1U, bmp.size(), f);
        (void)std::fclose(f);
        CRD_LOG_INFO(g_log_gizmo, "screenshot -> {} ({}x{})", path, w, h);
    }
}

// world-AABB extractor for the spatial index (the sandbox's, with the probe's smaller half-extent). ⛔ The BVH
// MUST be `configure`d before the world holds Transforms — an unconfigured index dereferences a null extractor on
// the first insert (an access violation on frame 1, which is exactly how this probe first failed).
struct ProbeExtractor final : crd::scene::IAabbExtractor
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

enum class Shapes : crd::u8
{
    Triad = 0, // the full RViz triad (3 arrows: stem line + 4-triangle cone head each)
    Stem,      // ONE bare line — no cone, no siblings: the minimal repro
    Arrow,     // ONE arrow — line + cone, the Y axis only
    All,       // the triad + the sandbox's wire/solid showcase shapes
    None       // NOTHING submitted — the control: whatever survives this is not the overlay's geometry
};
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Shapes                shapes           = Shapes::Triad;
    crd::f32              line_width_px    = 5.0F;
    crd::f32              cam_radius       = 7.0F;
    crd::f32              cam_height       = 2.6F;
    bool                  freeze           = false;
    bool                  want_grid        = true;
    bool                  want_backdrop    = true;
    bool                  want_validation  = true;
    bool                  smoke_test       = false;
    crd::f64              smoke_duration_s = 3.0;
    crd::f64              screenshot_at_s  = 2.5;
    const char*           screenshot_path  = nullptr;
    const char*           frame_asset      = "engine://frame/forward_csm_agx"; // RAF-12: selected by canonical id
    const char*           forward_tech     = nullptr; // `--technique unlit` bypasses the whole BRDF (a bisector)
    crd::gpu::PresentMode present_mode     = crd::gpu::PresentMode::Fifo;
    bool                  want_dx12        = false; // REN-39-D1: `--backend dx12` — the SAME app, second backend
    bool                  want_pull_draws  = false; // `--pull-draws`: the classic (non-indexed) draw path
    // ⛔ `--no-shadows` is a DIAGNOSTIC NECESSITY, not a nicety: with shadows active the renderer draws with its
    // SHADOWED program (cooked from `forward_csm`) no matter what technique the frame asset declares, so a probe
    // that edits `standard_forward` changes nothing and every reading taken from it is void.
    bool                  want_shadows     = true;

    for (int i = 1; i < argc; ++i)
    {
        const auto next_f32 = [&](crd::f32& dst) {
            if (i + 1 < argc)
            {
                char*          end = nullptr;
                const crd::f64 v   = std::strtod(argv[i + 1], &end);
                if (end != argv[i + 1]) { dst = static_cast<crd::f32>(v); }
                ++i;
            }
        };
        if (std::strcmp(argv[i], "--shapes") == 0 && i + 1 < argc)
        {
            ++i;
            if (std::strcmp(argv[i], "stem") == 0) { shapes = Shapes::Stem; }
            else if (std::strcmp(argv[i], "arrow") == 0) { shapes = Shapes::Arrow; }
            else if (std::strcmp(argv[i], "all") == 0) { shapes = Shapes::All; }
            else if (std::strcmp(argv[i], "none") == 0) { shapes = Shapes::None; }
            else { shapes = Shapes::Triad; }
        }
        else if (std::strcmp(argv[i], "--width") == 0) { next_f32(line_width_px); }
        else if (std::strcmp(argv[i], "--radius") == 0) { next_f32(cam_radius); }
        else if (std::strcmp(argv[i], "--height") == 0) { next_f32(cam_height); }
        else if (std::strcmp(argv[i], "--freeze") == 0) { freeze = true; }
        else if (std::strcmp(argv[i], "--pull-draws") == 0) { want_pull_draws = true; }
        else if (std::strcmp(argv[i], "--no-shadows") == 0) { want_shadows = false; }
        else if (std::strcmp(argv[i], "--no-grid") == 0) { want_grid = false; }
        else if (std::strcmp(argv[i], "--no-backdrop") == 0) { want_backdrop = false; }
        else if (std::strcmp(argv[i], "--no-validation") == 0) { want_validation = false; }
        else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc) { frame_asset = argv[++i]; }
        else if (std::strcmp(argv[i], "--technique") == 0 && i + 1 < argc) { forward_tech = argv[++i]; }
        else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
        {
            ++i;
            want_dx12 = std::strcmp(argv[i], "dx12") == 0;
        }
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) { screenshot_path = argv[++i]; }
        else if (std::strcmp(argv[i], "--screenshot-at") == 0 && i + 1 < argc)
        {
            char*          end = nullptr;
            const crd::f64 v   = std::strtod(argv[i + 1], &end);
            if (end != argv[i + 1] && v > 0.0) { screenshot_at_s = v; }
            ++i;
        }
        else if (std::strcmp(argv[i], "--present") == 0 && i + 1 < argc)
        {
            ++i;
            if (std::strcmp(argv[i], "immediate") == 0) { present_mode = crd::gpu::PresentMode::Immediate; }
            else if (std::strcmp(argv[i], "mailbox") == 0) { present_mode = crd::gpu::PresentMode::Mailbox; }
            else { present_mode = crd::gpu::PresentMode::Fifo; }
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

    crd::log::LoggerConfig lcfg;
    lcfg.async = false;
    crd::log::init(lcfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    crd::app::ApplicationDesc app_desc;
    app_desc.window.title = crd::containers::String("Cerid Gizmo Probe — the axis triad, close up");
    app_desc.window.size  = {1280, 720};
    crd::app::Application app(app_desc);
    if (!app.is_valid())
    {
        CRD_LOG_ERROR(g_log_gizmo, "Application init failed");
        crd::log::shutdown();
        return 1;
    }

    // ⭐⭐ REN-39-D1: ONE app, EITHER backend. Everything downstream of these two factory calls is the portable
    // interface (`IGpuContext` / `IRasterContext`), which is the whole point of ADR-0105 — and the reason the
    // gizmo can now be compared pixel-for-pixel across backends by a command-line flag.
    std::unique_ptr<crd::gpu::IGpuContext>    gpu_context;
    std::unique_ptr<crd::gpu::IRasterContext> raster;
    if (want_dx12)
    {
#ifdef _WIN32
        gpu_context = crd::gpu::create_dx12_gpu_context();
        if (gpu_context == nullptr || !gpu_context->valid())
        {
            CRD_LOG_ERROR(g_log_gizmo, "DX12 GPU bootstrap failed");
            crd::log::shutdown();
            return 1;
        }
        raster = crd::gpu::create_dx12_raster_context();
#else
        CRD_LOG_ERROR(g_log_gizmo, "--backend dx12 is Windows-only (the D3D12 backend is not built on this platform)");
        crd::log::shutdown();
        return 1;
#endif
    }
    else
    {
        crd::gpu::GpuContextConfig gpu_cfg;
        gpu_cfg.backend           = crd::gpu::GpuBackend::Vulkan;
        gpu_cfg.headless          = false;
        gpu_cfg.enable_validation = want_validation;
        gpu_context               = crd::gpu::create_vulkan_gpu_context(gpu_cfg);
        auto* vk = gpu_context != nullptr ? static_cast<crd::gpu::VulkanGpuContext*>(gpu_context.get()) : nullptr;
        if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
        {
            CRD_LOG_ERROR(g_log_gizmo, "Vulkan GPU bootstrap failed");
            crd::log::shutdown();
            return 1;
        }
        raster = crd::gpu::create_vulkan_raster_context(*vk);
    }
    if (raster == nullptr || !raster->valid())
    {
        CRD_LOG_ERROR(g_log_gizmo, "Raster context unavailable");
        crd::log::shutdown();
        return 1;
    }
    CRD_LOG_INFO(g_log_gizmo, "backend: {}", want_dx12 ? "DX12" : "Vulkan");
    const auto fb    = app.window().framebuffer_size();
    crd::u32   win_w = fb.width > 0 ? static_cast<crd::u32>(fb.width) : 1280U;
    crd::u32   win_h = fb.height > 0 ? static_cast<crd::u32>(fb.height) : 720U;
    auto       surface = raster->create_present_surface(native_window_of(app), win_w, win_h, present_mode);
    if (surface == nullptr)
    {
        CRD_LOG_ERROR(g_log_gizmo, "Present surface creation failed");
        crd::log::shutdown();
        return 1;
    }

    // ── the scene: BACKDROP geometry only. The subject is the overlay; the meshes exist so the authored graph's
    // geometry pass has draws (and so the gizmo is seen against real, depth-tested geometry — the condition the
    // ghost needs).
    // ⛔ 256 MB, like the sandbox: the mounted PACK's mesh/material/texture payloads live here, and they are sized
    // by the ASSET LIBRARY, not by how many instances this probe spawns. 64 MB asserted out at mount.
    crd::memory::TlsfAllocator scene_alloc(256U << 20U);
    crd::resources::ResourceManager rm(&scene_alloc);
    crd::resources::register_mesh_loader(&rm, nullptr);
    crd::resources::register_openpbr_material_loader(&rm, nullptr);
    crd::resources::register_texture_loader(&rm, nullptr);
    const crd::platform::fs::Path pack_path =
        crd::platform::fs::executable_dir() / crd::containers::StringView(CRD_DEMO_ASSETS_REL_PACK);
    crd::containers::Array<crd::resources::ResourceId> pack_meshes(&scene_alloc);
    if (rm.mount_manifest(pack_path.generic()).is_valid())
    {
        collect_pack_meshes(pack_path, &scene_alloc, pack_meshes);
    }
    if (pack_meshes.size() == 0U)
    {
        CRD_LOG_ERROR(g_log_gizmo, "demo pack has no meshes at '{}' — the graph needs one draw", pack_path.generic());
        crd::log::shutdown();
        return 1;
    }

    crd::scene::World world{&scene_alloc};
    world.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait(),
                                                    crd::scene::SpatialBVH{});
    crd::scene::register_render_components(world);
    auto*          bvh = world.find_index<crd::scene::SpatialBVHIndex>();
    ProbeExtractor extractor;
    if (bvh != nullptr)
    {
        bvh->configure(&extractor,
                       crd::geometry::spatial::OctreeBuildOptions<crd::f32>{
                           crd::geometry::primitives::AABB3<crd::f32>{{-40, -20, -40}, {40, 20, 40}}, 1.0F, 8U, 8U});
    }

    // a 5x5 backdrop patch at 2.5 m spacing, each mesh normalized to ~1.2 units — dense enough to sit BEHIND the
    // triad from every orbit angle, sparse enough that the gizmo is never hidden.
    {
        const crd::u32 side = want_backdrop ? 5U : kBackdropMin;
        crd::u32       n    = 0U;
        for (crd::u32 iz = 0; iz < side; ++iz)
        {
            for (crd::u32 ix = 0; ix < side; ++ix)
            {
                const crd::resources::ResourceId mid = pack_meshes[n % pack_meshes.size()];
                auto        handle = rm.load_sync<crd::resources::MeshResource>(mid);
                const auto* mesh   = handle.get();
                crd::f32    scale  = 1.0F;
                if (mesh != nullptr && mesh->has_bounds())
                {
                    const crd::f32 ex = mesh->bounds_max[0] - mesh->bounds_min[0];
                    const crd::f32 ey = mesh->bounds_max[1] - mesh->bounds_min[1];
                    const crd::f32 ez = mesh->bounds_max[2] - mesh->bounds_min[2];
                    crd::f32       mx = ex > ey ? ex : ey;
                    mx                = mx > ez ? mx : ez;
                    if (mx > 1.0e-6F) { scale = 1.2F / mx; }
                }
                crd::resources::ResourceId material{};
                if (mesh != nullptr && mesh->primitives.size() > 0U) { material = mesh->primitives[0].material_id; }

                const crd::f32 x = (static_cast<crd::f32>(ix) - static_cast<crd::f32>(side - 1U) * 0.5F) * 2.5F;
                const crd::f32 z = (static_cast<crd::f32>(iz) - static_cast<crd::f32>(side - 1U) * 0.5F) * 2.5F;
                // ⛔ the CENTRE cell is left empty: the triad's origin must not be inside a mesh
                if (side > 1U && ix == side / 2U && iz == side / 2U) { continue; }

                const crd::scene::EntityId e = world.spawn();
                crd::scene::Transform      t;
                t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{x, 0.0F, z});
                t.rotation    = crd::math::from_axis_angle(crd::math::Vec3f{0, 1, 0}, 0.0F);
                t.scale       = {scale, scale, scale};
                t.world       = crd::math::from_trs(crd::math::Vec3f{x, 0.0F, z}, t.rotation, t.scale);
                world.add_component(e, t);
                world.add_component(e, crd::scene::MeshRenderer{mid, material});
                ++n;
            }
        }
        CRD_LOG_INFO(g_log_gizmo, "backdrop: {} instance(s) from {} pack mesh(es)", n, pack_meshes.size());
    }

    // ── the renderer: OUR authored assets. Root first (materials/lighting/vertex/post resolve disk-first at cook
    // time), then programs, then the frame BY NAME — identical to the sandbox's contract.
    auto  renderer_ptr = std::make_unique<crd::scenerender::SceneRenderer>(&scene_alloc);
    auto& renderer     = *renderer_ptr;
    if (const char* aroot = probe_getenv("CRD_ASSETS_DIR"); aroot != nullptr && aroot[0] != 0)
    {
        const bool root_ok = renderer.set_asset_root(aroot);
        CRD_LOG_INFO(g_log_gizmo, "asset root '{}' -> {}", aroot, root_ok ? "installed" : "REJECTED");
    }
    else
    {
        CRD_LOG_WARN(g_log_gizmo, "CRD_ASSETS_DIR unset — running on the EMBEDDED asset pack");
    }
    // ⭐ the bisector: `--technique unlit` shades from base_color + emissive ALONE — no normal, no view vector,
    // no light. If a backend renders black under the full BRDF but correct under `unlit`, the defect is in the
    // LIGHTING INPUTS (the varyings feeding it), not in the material or the geometry.
    if (forward_tech != nullptr) { renderer.set_forward_technique(forward_tech); }
    const bool scene_ready = renderer.init(*raster, rm) && renderer.init_programs(*gpu_context);
    if (!scene_ready)
    {
        CRD_LOG_ERROR(g_log_gizmo, "SceneRenderer init failed");
        crd::log::shutdown();
        return 1;
    }
    renderer.set_readback_enabled(screenshot_path != nullptr);
    // the REN-39 A/B: indexed pull binds the storage buffer to the FS through a READ-ONLY view (t0 on DX12,
    // a separate root table); the classic path leaves the FS on the same u0 the VS uses.
    if (want_pull_draws) { renderer.set_indexed_pull(false); }
    {
        crd::scenerender::CsmConfig ccfg;
        ccfg.cascade_count = 4;
        ccfg.map_size      = 2048;
        ccfg.far_plane     = 40.0F; // the probe's world is ~12 units across
        renderer.set_csm_config(ccfg);
        const bool shadows_on = renderer.set_shadows_enabled(want_shadows);
        CRD_LOG_INFO(g_log_gizmo, "cascaded shadows: {}", shadows_on ? "ON" : "unavailable");
    }
    {
        const bool ok = renderer.set_frame_graph(frame_asset);
        CRD_LOG_INFO(g_log_gizmo, "frame '{}' -> {}", frame_asset, ok ? "installed" : "REJECTED");
        if (!ok)
        {
            crd::log::shutdown();
            return 1;
        }
    }

    auto canvas = raster->create_color_depth_target(surface->width(), surface->height());
    if (canvas == nullptr)
    {
        CRD_LOG_ERROR(g_log_gizmo, "Canvas creation failed");
        crd::log::shutdown();
        return 1;
    }

    // ── the SUBJECT: the debug-draw primitives, submitted once (immediate-mode lifetime 0 would clear them, so
    // the buffer is built once and re-submitted every frame — exactly the sandbox's pattern).
    const bool draw_ready = crd::draw::init(*gpu_context, *raster);
    if (!draw_ready)
    {
        CRD_LOG_ERROR(g_log_gizmo, "crd-draw init failed");
        crd::log::shutdown();
        return 1;
    }
    crd::draw::RenderBuffer draw_buf(crd::memory::default_allocator());
    switch (shapes)
    {
    case Shapes::Stem:
        crd::draw::add_line_to(draw_buf, {0.0F, 0.0F, 0.0F}, {0.0F, 2.4F, 0.0F}, crd::draw::kAxisY, line_width_px);
        break;
    case Shapes::Arrow:
        crd::draw::arrow_to(draw_buf, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 3.0F, crd::draw::kAxisY, 0.2F, 0.4F,
                            line_width_px);
        break;
    case Shapes::All:
        crd::draw::axis_triad_to(draw_buf, crd::math::Mat4f::identity(), 3.0F, 5.0F);
        crd::draw::sphere_wire_to(draw_buf, {4.0F, 1.2F, 0.0F}, 1.0F, crd::draw::kCyan);
        crd::draw::capsule_wire_to(draw_buf, {-4.0F, 0.6F, 0.0F}, {-4.0F, 2.2F, 0.0F}, 0.6F, crd::draw::kMagenta);
        break;
    case Shapes::None:
        break; // the control arm — the overlay pass still runs, it just has nothing to draw
    case Shapes::Triad:
    default:
        crd::draw::axis_triad_to(draw_buf, crd::math::Mat4f::identity(), 3.0F, 5.0F);
        break;
    }

    // the overlay rides the authored graph as a PASS (the hard rule), drawing the image ITS PASS DECLARED
    struct OverlayCtx
    {
        crd::draw::RenderBuffer*         buf      = nullptr;
        crd::gpu::IRasterTarget*         target   = nullptr;
        crd::scenerender::SceneRenderer* renderer = nullptr;
        crd::draw::OverlayPassConfig     cfg{};
    } overlay_ctx{&draw_buf, canvas.get(), &renderer, {}};
    renderer.set_overlay_pass(
        [](crd::gpu::IFrameContext& ctx, void* user) {
            auto*                    o = static_cast<OverlayCtx*>(user);
            crd::gpu::IRasterTarget* t = o->renderer != nullptr ? o->renderer->overlay_target(ctx) : nullptr;
            if (t == nullptr) { t = o->target; }
            if (!crd::draw::submit_overlay(*t, *o->buf, o->cfg))
            {
                CRD_LOG_WARN(g_log_gizmo, "draw overlay submission refused");
            }
        },
        &overlay_ctx);

    // ⛔ the chunk-grain sync runs parallel_for — an uninitialised job system is an access violation on frame 1
    crd::jobs::init(app_desc.jobs_config);

    CRD_LOG_INFO(g_log_gizmo, "gizmo probe up: radius {:.1f} height {:.1f} shapes={} grid={} freeze={}", cam_radius,
                 cam_height, static_cast<int>(shapes), want_grid, freeze);

    crd::u32       frame               = 0;
    crd::u32       frames_with_present = 0;
    const auto     t_start             = std::chrono::steady_clock::now();
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

        const crd::f64 tsec = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t_start).count();
        const float    orbit = freeze ? 0.6F : static_cast<float>(tsec * 0.35);
        const crd::math::Vec3f eye{crd::math::sin(orbit) * cam_radius, cam_height,
                                   crd::math::cos(orbit) * cam_radius};
        // look at the MIDDLE of the triad, so a 3-unit gizmo fills the frame at radius 7
        const crd::math::Vec3f target{0.0F, 1.4F, 0.0F};
        const crd::math::Mat4f view = crd::math::look_at(eye, target, crd::math::Vec3f{0.0F, 1.0F, 0.0F});
        const float            aspect =
            static_cast<float>(surface->width()) / static_cast<float>(surface->height() > 0U ? surface->height() : 1U);
        const crd::math::Mat4f proj = crd::math::perspective_reverse_z(1.0472F, aspect, 0.05F);
        const crd::math::Mat4f vp   = proj * view;

        // ⛔ the overlay CONFIG must be current BEFORE render(): the woven overlay pass records INSIDE it.
        overlay_ctx.target          = canvas.get();
        overlay_ctx.cfg.view_proj   = vp;
        overlay_ctx.cfg.viewport_px = {static_cast<crd::f32>(surface->width()),
                                       static_cast<crd::f32>(surface->height())};
        overlay_ctx.cfg.time_s      = static_cast<crd::f32>(tsec);
        overlay_ctx.cfg.depth_test  = crd::gpu::DepthCompare::GreaterEqual; // reverse-Z canvas
        overlay_ctx.cfg.grid.enabled    = want_grid;
        overlay_ctx.cfg.grid.camera_pos = eye;
        overlay_ctx.cfg.grid.apply_theme();

        (void)renderer.sync(world);
        const crd::scenerender::RenderStats stats =
            renderer.render(*canvas, vp, crd::math::Vec3f{0.35F, 1.0F, 0.25F},
                            crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F}, bvh);

        if (screenshot_path != nullptr && tsec >= screenshot_at_s)
        {
            write_bmp(screenshot_path, *canvas);
            app.close();
        }
        if (frame % 60U == 0U)
        {
            CRD_LOG_INFO(g_log_gizmo, "frame {}: {} draw(s) {} instance(s) | gpu {:.3f} ms ({} passes)", frame,
                         stats.draws, stats.drawn_instances, stats.gpu_ms, stats.timed_passes);
        }

        if (surface->present(*canvas)) { ++frames_with_present; }
        ++frame;

        if (smoke_test && tsec >= smoke_duration_s)
        {
            if (frames_with_present == 0)
            {
                CRD_LOG_ERROR(g_log_gizmo, "Smoke-test: 0 frames presented in {:.2f}s — failure", tsec);
                crd::log::flush();
                crd::log::shutdown();
                return 2;
            }
            CRD_LOG_INFO(g_log_gizmo, "Smoke-test: PASS — {} frames over {:.2f}s ({:.1f} fps)", frames_with_present,
                         tsec, static_cast<crd::f64>(frames_with_present) / tsec);
            app.close();
        }
        if (app.window().input().state().was_key_pressed(crd::platform::Key::Escape)) { app.close(); }
    }

    // teardown order mirrors the sandbox: surface (drains frames in flight) → renderer → draw
    crd::jobs::shutdown();
    surface.reset();
    renderer_ptr.reset();
    crd::draw::shutdown();
    canvas.reset();
    raster.reset();
    gpu_context.reset();
    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
