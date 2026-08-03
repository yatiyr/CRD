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
#include <crd/gpu/dx12_context.hpp>        // REN-39-D2: `--backend dx12` — the sandbox on the second backend
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/imgui/imgui_gpu_backend.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/log/log.hpp>
#include <crd/math/cmath.hpp>
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

// C4996-safe env read (the engine's `mf_getenv` pattern): a FIXED, documented dev knob, not a security
// surface. The sandbox honours `CRD_ASSETS_DIR` so a shipped `assets/**` tree shadows the embedded pack.
[[nodiscard]] inline const char* sandbox_getenv(const char* name) noexcept
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
    bool                  want_shadows   = true;
    bool                  want_pcss      = true; // PCSS contact-hardening is the default; --hard-shadows = PCF
    // Validation is the right dev DEFAULT, but its CPU cost is real (descriptor tracking dominates at high draw
    // counts) and a SHIPPED app never runs it — a perf number with validation on measures a config nothing
    // ships (the gates-ran-unshipped-config scar). `--no-validation` is the honest arm for any speed claim.
    bool                  want_validation = true;
    bool                  want_dx12       = false; // REN-39-D2: `--backend dx12` — the SAME sandbox, D3D12
    bool want_pull_draws = false; // REN-39-C2: the A/B baseline arm (indexed is the default)
    bool want_gpu_cull   = false; // REN-40-A: the device-side cull arm (CPU cull is the default)
    bool want_gpu_skin   = false; // REN-40-F: device-side bone palette (CPU palette is the default)
    bool want_no_bvh     = false; // REN-40-A: drop the CPU cull's BVH broad phase (attribution)
    // ⭐⭐ REN-40-C2: build LOD chains from an authored `.crdlod` policy. ⛔ OFF by default and named on the
    // command line for the same reason `--gpu-cull` is: it changes what is DRAWN, so the A/B has to run on one
    // build. `--lod <asset>` overrides the shipped default policy.
    const char* lod_asset = nullptr;
    bool        want_lod  = false;
    // ⭐⭐ REN-40-C2: the LOD SHOWCASE scene (see where it is built) — ONE mesh, a line receding from the camera,
    // no grid / no monuments / no animated ring. `--instances` is the line's length.
    bool        want_lod_showcase = false;
    // ⭐⭐ REN-40-C2: `--lod-override-probe` gives every OTHER showcase instance a `MeshLodOverride` pinned to
    // the coarsest level — the optional component made visible. Off by default so the forced-level sheets stay
    // a clean picture of the LEVELS themselves.
    bool        want_lod_override_probe = false;
    bool want_verify     = false; // REN-40-A: run the CPU cull too, for the count comparison
    const char* frame_override = nullptr; // REN-40-A: install this authored frame graph by name
    crd::f64    fixed_dt_ms    = 0.0;     // REN-40-A: >0 ⇒ deterministic clock (frame counter × dt)
    const char* screenshot_path = nullptr; // REN-39: dump the canvas at ~2.5 s and exit
    crd::f64    screenshot_at_s = 2.5;     // `--screenshot-at`: pick the moment in the camera loop
    // ⭐⭐ REN-40: the scene SIZE is a knob so the scaling curve is measured, not argued. `--instances N` is a
    // TOTAL (rounded to the enclosing square grid); `--foxes N` sets the SKINNED ring.
    crd::u32    grid_side  = 100U; // 100×100 = the historical 10k
    crd::u32    fox_count  = 24U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--headless") == 0) { headless = true; }
        else if (std::strcmp(argv[i], "--instances") == 0 && i + 1 < argc)
        {
            const long v = std::strtol(argv[++i], nullptr, 10);
            if (v > 0)
            {
                // round-to-nearest without the (x + 0.5) cast the tidy gate rejects: walk the floor up while
                // the next side still fits the requested total
                auto s = static_cast<crd::u32>(crd::math::sqrt(static_cast<crd::f32>(v)));
                while (static_cast<long>(s + 1U) * static_cast<long>(s + 1U) <= v) { ++s; }
                if (s < 1U) { s = 1U; }
                grid_side = s;
            }
        }
        else if (std::strcmp(argv[i], "--foxes") == 0 && i + 1 < argc)
        {
            const long v = std::strtol(argv[++i], nullptr, 10);
            fox_count    = v > 0 ? static_cast<crd::u32>(v) : 0U;
        }
        // REN-8: A/B the per-frame readback copy. Run-to-run fps varies by ~10 on this host, so a claim like
        // "removing the readback made it faster" is only honest if BOTH arms are measured on the same build.
        else if (std::strcmp(argv[i], "--readback") == 0) { force_readback = true; }
        // REN-3.2-b: A/B the shadows. If an object looks unlit with shadows ON and STILL looks unlit with them
        // OFF, the cause is its NORMALS (ndl == 0 gives the same flat ambient as vis == 0), not the shadow map.
        else if (std::strcmp(argv[i], "--no-shadows") == 0) { want_shadows = false; }
        // PCSS (contact-hardening soft shadows) is the DEFAULT; `--hard-shadows` keeps fixed-radius PCF so the
        // A/B measures both arms on one build (the same rule every other quality flag follows here).
        else if (std::strcmp(argv[i], "--hard-shadows") == 0) { want_pcss = false; }
        // ⭐⭐ REN-39-C2: A/B the draw path. INDEXED is the default (post-transform vertex reuse — the frame was
        // measured VERTEX-bound); `--pull-draws` keeps the classic pull so the before/after board measures BOTH
        // arms on the SAME build (the readback A/B rule, one flag over).
        else if (std::strcmp(argv[i], "--pull-draws") == 0)
        {
            want_pull_draws = true;
        }
        // ⭐⭐ REN-40-A: `--gpu-cull` runs the frustum cull (camera + every cascade) ON THE DEVICE through the
        // authored `forward_csm_gpu` graph. ⛔ Default OFF so the A/B measures BOTH arms on ONE build.
        else if (std::strcmp(argv[i], "--gpu-cull") == 0) { want_gpu_cull = true; }
        // ⛔ `--gpu-cull-verify` keeps the CPU cull running beside the device one so the two verdicts can be
        // compared in ONE frame. It gives up the speedup on purpose — it is the correctness arm, not the fast one.
        else if (std::strcmp(argv[i], "--gpu-cull-verify") == 0) { want_gpu_cull = want_verify = true; }
        // ⭐⭐ REN-40-F: `--gpu-skin` moves the bone palette to the device. ⛔ Requires the GPU frame graph
        // (the `gpu_skin` compute pass lives there), so it forces the same TOML as `--gpu-cull`.
        else if (std::strcmp(argv[i], "--gpu-skin") == 0) { want_gpu_skin = true; }
        // ⛔ REN-40-A: `--no-bvh` drops the CPU cull's BVH BROAD PHASE. The device cull brute-forces every
        // instance, so this is how a GPU-vs-CPU count disagreement is attributed: if it vanishes here, the broad
        // phase was the one dropping geometry, not the kernel.
        else if (std::strcmp(argv[i], "--no-bvh") == 0) { want_no_bvh = true; }
        // ⭐⭐ REN-40-C2: `--lod [asset]` turns on discrete LOD chains. The shipped default policy is
        // `lod/scene_default.crdlod` (0.5 / 0.25 / 0.08 at 512 / 128 / 40 px).
        else if (std::strcmp(argv[i], "--lod-showcase") == 0) { want_lod_showcase = true; }
        else if (std::strcmp(argv[i], "--lod-override-probe") == 0) { want_lod_override_probe = true; }
        else if (std::strcmp(argv[i], "--lod") == 0)
        {
            want_lod = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') { lod_asset = argv[++i]; }
        }
        // ⛔ REN-40-A: `--frame <asset>` installs an authored frame graph BY NAME, independent of every other
        // switch. It is how "is the ASSET wrong?" gets separated from "is the FEATURE wrong?" — the two questions
        // a combined flag makes indistinguishable.
        else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc) { frame_override = argv[++i]; }
        // ⛔⛔ REN-40-A: `--fixed-dt <ms>` drives the clock from the FRAME COUNTER, not the wall clock.
        // Without it two runs of the same scene land on DIFFERENT camera poses at the same `--screenshot-at`
        // (the frame rate differs), so an A/B pixel comparison measures the camera, not the change. Two images
        // that differ by half a duck's width read exactly like a lighting regression — that misreading cost real
        // time here. With a fixed dt the run is deterministic and the two arms are comparable pixel for pixel.
        else if (std::strcmp(argv[i], "--fixed-dt") == 0 && i + 1 < argc)
        {
            fixed_dt_ms = std::strtod(argv[++i], nullptr);
        }
        // REN-39: "what does the user SEE", as a file — the presented canvas at ~2.5 s, then exit.
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) { screenshot_path = argv[++i]; }
        // `--screenshot-at <sec>`: capture at a chosen point of the 20 s camera loop (e.g. the low close pass)
        else if (std::strcmp(argv[i], "--screenshot-at") == 0 && i + 1 < argc)
        {
            char*          end = nullptr;
            const crd::f64 v   = std::strtod(argv[i + 1], &end);
            if (end != argv[i + 1] && v > 0.0)
            {
                screenshot_at_s = v;
                ++i;
            }
        }
        else if (std::strcmp(argv[i], "--no-validation") == 0) { want_validation = false; }
        else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
        {
            ++i;
            want_dx12 = std::strcmp(argv[i], "dx12") == 0;
        }
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
    // ⭐⭐ REN-39-D2: EITHER backend. Everything past these two factory calls is the portable interface — which is
    // the whole claim of ADR-0105, and now the sandbox is what proves it rather than asserts it.
    std::unique_ptr<crd::gpu::IGpuContext>    gpu_context;
    std::unique_ptr<crd::gpu::IRasterContext> raster;
    if (want_dx12)
    {
        gpu_context = crd::gpu::create_dx12_gpu_context();
        if (gpu_context == nullptr || !gpu_context->valid())
        {
            CRD_LOG_ERROR(g_log_sandbox, "DX12 GPU bootstrap failed");
            crd::log::shutdown();
            return 1;
        }
        raster = crd::gpu::create_dx12_raster_context();
    }
    else
    {
        crd::gpu::GpuContextConfig gpu_cfg;
        gpu_cfg.backend           = crd::gpu::GpuBackend::Vulkan;
        gpu_cfg.headless          = false;
        gpu_cfg.enable_validation = !headless && want_validation;
        gpu_context               = crd::gpu::create_vulkan_gpu_context(gpu_cfg);
        auto* vkc = gpu_context != nullptr ? static_cast<crd::gpu::VulkanGpuContext*>(gpu_context.get()) : nullptr;
        if (vkc == nullptr || !vkc->graphics_capable() || !vkc->shader_object())
        {
            CRD_LOG_ERROR(g_log_sandbox, "GPU bootstrap failed — no graphics-capable Vulkan device / shader objects");
            crd::log::shutdown();
            return 1;
        }
        raster = crd::gpu::create_vulkan_raster_context(*vkc);
    }
    if (raster == nullptr || !raster->valid())
    {
        CRD_LOG_ERROR(g_log_sandbox, "Raster context unavailable");
        crd::log::shutdown();
        return 1;
    }
    CRD_LOG_INFO(g_log_sandbox, "backend: {}", want_dx12 ? "DX12" : "Vulkan");
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
    // ⭐⭐ REN-40: the arena SCALES WITH THE SCENE. At 1M instances the CPU-side scene alone wants ~300 MB (the
    // World's Transform + MeshRenderer components, the renderer's InstanceGpu payload, per-slot world AABBs, the
    // camera visible list and FOUR cascade lists) — a fixed 256 MB asserted "out of memory" before the first
    // frame. Budget per instance, generously, rather than tuning a constant per scene size.
    // ⭐⭐ REN-41 (velocity): +256 B/instance over the old 512 — the per-instance PREVIOUS world transform is
    // 64 B (16 floats), and an Array grows by doubling, so its final reallocation transiently holds old+new
    // (another ~64 B/instance peak). The rest is margin; at 1M this is a ~960 MB arena.
    const crd::u64 inst_total  = static_cast<crd::u64>(grid_side) * grid_side + fox_count;
    const crd::u64 arena_bytes = (192ULL << 20U) + inst_total * 768ULL;
    crd::memory::TlsfAllocator scene_alloc(static_cast<crd::usize>(arena_bytes));
    CRD_LOG_INFO(g_log_sandbox, "scene arena: {} MiB for {} instances", arena_bytes >> 20U, inst_total);

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
    // ⭐⭐ REN-40: the grid SIDE and the skinned ring COUNT are knobs, so the scaling curve can be MEASURED
    // (`--instances 1000000` → a 1000×1000 grid) instead of argued about. Spacing shrinks with the side so the
    // field keeps its world extent and the cascade fit stays comparable across counts.
    const crd::u32 side = grid_side;
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
            // REN-3.2-b DIAGNOSTIC: print this monument's NORMAL health. Two hypotheses about the unlit torus
            // were both wrong; this reports the fact. mean_len ~0 => generation failed; outward_frac low =>
            // normals point INWARD (dot(N,L) <= 0 everywhere, which renders as flat ambient = "always shadowed").
            {
                const auto*      mr     = handle.get();
                const crd::usize stride = crd::resources::kMeshVertexStride;
                const crd::usize vc     = mr->vertices.size() / stride;
                double           sumlen = 0.0;
                double           cx = 0.0;
                double           cy = 0.0;
                double           cz = 0.0;
                for (crd::usize v = 0; v < vc; ++v)
                {
                    const auto* f = reinterpret_cast<const crd::f32*>(mr->vertices.data() + v * stride);
                    cx += static_cast<double>(f[0]); cy += static_cast<double>(f[1]); cz += static_cast<double>(f[2]);
                }
                if (vc > 0U) { cx /= static_cast<double>(vc); cy /= static_cast<double>(vc); cz /= static_cast<double>(vc); }
                crd::usize outward = 0;
                for (crd::usize v = 0; v < vc; ++v)
                {
                    const auto* f = reinterpret_cast<const crd::f32*>(mr->vertices.data() + v * stride);
                    const double nx = static_cast<double>(f[3]);
                    const double ny = static_cast<double>(f[4]);
                    const double nz = static_cast<double>(f[5]);
                    const double nl = crd::math::sqrt(nx * nx + ny * ny + nz * nz);
                    sumlen += nl;
                    const double d = static_cast<double>(f[3]) * (static_cast<double>(f[0]) - cx)
                                     + static_cast<double>(f[4]) * (static_cast<double>(f[1]) - cy)
                                     + static_cast<double>(f[5]) * (static_cast<double>(f[2]) - cz);
                    if (d > 0.0) { ++outward; }
                }
                CRD_LOG_INFO(g_log_sandbox,
                             "[normal-check] {} verts={} mean_len={:.4f} outward={:.1f}%",
                             pack_mesh_names[mi].c_str(), vc,
                             vc > 0U ? sumlen / static_cast<double>(vc) : 0.0,
                             vc > 0U ? 100.0 * static_cast<double>(outward) / static_cast<double>(vc) : 0.0);
            }
        }
        else { static_meshes.push_back(pack_meshes[mi]); }
    }
    // ── ⭐⭐ REN-40-C2: THE LOD SHOWCASE — a scene built to SEE a level chain, and nothing else. ──────────────
    // ⛔ WHY IT IS ITS OWN SCENE. The 10k/1M grid is the wrong instrument for judging LOD: it mixes seven meshes,
    // an animated ring and a monument circle, and every object is a few pixels tall — a coarse level is
    // literally too small to look at. Two real defects (every level published the NEXT level's switch height,
    // and 12-triangle cubes decimated to a degenerate 6, so the cubes vanished) survived a bit-identical parity
    // gate, a device-vs-CPU count gate and an fps board, and were both obvious within seconds of rendering ONE
    // mesh large. The probe scene is therefore a deliverable, not a debugging aid.
    //
    // It is ONE mesh — the highest-triangle one in the pack, the only kind a chain can meaningfully simplify —
    // repeated along +Z at a fixed spacing, with the camera at the near end looking down the line. That gives
    // both pictures at once: the NEAREST instance is large enough to judge the geometry, and the line receding
    // into the distance shows WHERE the switches happen and whether they are stable.
    if (want_lod_showcase && pack_meshes.size() > 0U && static_meshes.size() > 0U)
    {
        crd::resources::ResourceId best{};
        crd::u32                   best_tris = 0U;
        for (const auto& mid : static_meshes)
        {
            auto h = rm.load_sync<crd::resources::MeshResource>(mid);
            if (h.state() != crd::resources::LoadState::Ready || h.get() == nullptr) { continue; }
            const auto tris = static_cast<crd::u32>(h.get()->indices.size() / 12U);
            if (tris > best_tris)
            {
                best_tris = tris;
                best      = mid;
            }
        }
        auto handle = rm.load_sync<crd::resources::MeshResource>(best);
        if (handle.state() == crd::resources::LoadState::Ready && handle.get() != nullptr)
        {
            const auto* mesh  = handle.get();
            crd::f32    scale = 1.0F;
            if (mesh->has_bounds())
            {
                const crd::f32 ex = mesh->bounds_max[0] - mesh->bounds_min[0];
                const crd::f32 ey = mesh->bounds_max[1] - mesh->bounds_min[1];
                const crd::f32 ez = mesh->bounds_max[2] - mesh->bounds_min[2];
                crd::f32       m  = ex > ey ? ex : ey;
                m                 = m > ez ? m : ez;
                if (m > 1.0e-6F) { scale = 2.0F / m; }
            }
            crd::resources::ResourceId material{};
            if (mesh->primitives.size() > 0U) { material = mesh->primitives[0].material_id; }
            CRD_LOG_INFO(g_log_sandbox, "LOD showcase: {} instances of the {}-triangle mesh, spaced 3.0 along +Z",
                         inst_total, best_tris);
            for (crd::u32 i = 0; i < inst_total; ++i)
            {
                const crd::f32             z = static_cast<crd::f32>(i) * 3.0F;
                const crd::scene::EntityId e = world.spawn();
                crd::scene::Transform      t;
                t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{0.0F, 0.0F, z});
                t.scale       = {scale, scale, scale};
                t.world = crd::math::from_trs(crd::math::Vec3f{0.0F, 0.0F, z}, crd::math::Quatf::identity(), t.scale);
                world.add_component(e, t);
                world.add_component(e, crd::scene::MeshRenderer{best, material});
                // ⭐⭐ REN-40-C2: EVERY OTHER instance carries a per-entity override that PINS it to the coarsest
                // level. ⛔ It is the probe that makes the optional component provable: a bias or a clamp that did
                // nothing would leave the line uniform, and a line that alternates fine/coarse down its length is
                // something you can SEE — the same discipline the forced-level policies use, one scope down.
                if (want_lod_override_probe && (i & 1U) != 0U)
                {
                    crd::scene::MeshLodOverride ov;
                    ov.screen_bias = 1.0F;
                    ov.min_level   = 5U; // pinned to the coarsest declared level
                    ov.max_level   = 7U;
                    world.add_component(e, ov);
                }
                cells.push_back(Cell{e, 0.0F, z, scale});
            }
        }
    }
    else if (pack_meshes.size() > 0U && static_meshes.size() > 0U)
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

                const crd::f32 half = (static_cast<crd::f32>(side) - 1.0F) * 0.5F;
                const crd::f32 sp   = 2.0F; // FIXED SPACING: the world GROWS with the count, so a
                const crd::f32 x    = (static_cast<crd::f32>(gx) - half) * sp; // 1M => 2000 units
                const crd::f32 z    = (static_cast<crd::f32>(gz) - half) * sp;

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
    // ⛔ REN-40-C2: the showcase is ONE mesh and nothing else — a skinned ring and a monument circle in the frame
    // are exactly the noise that hid the defect.
    if (!want_lod_showcase && skinned_meshes.size() > 0U && pack_skeletons.size() > 0U)
    {
        const crd::u32 ring_count = fox_count;
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
    // REN-3.2-b DIAGNOSTIC: report each monument mesh's NORMAL health at load. Two guesses about why the
    // torus renders unlit were both wrong; this prints the fact instead — mean normal length (0 => generation
    // failed) and the fraction pointing OUTWARD from the mesh centroid (low => inward winding).
    // the GEO MONUMENTS: every non-glTF import (STL icosahedron · OBJ torus with GENERATED normals · the teal
    // 3MF box whose displaycolor travelled sRGB→linear→PBRM→PRIM→this frame) — large, slowly spinning
    struct Monument
    {
        crd::scene::EntityId entity;
        crd::f32             x, z, scale;
    };
    crd::containers::Array<Monument> monuments(&scene_alloc);
    for (crd::usize mi = 0; mi < (want_lod_showcase ? crd::usize{0} : monument_meshes.size()); ++mi)
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
    // ⛔ 38-G1: the asset root MUST install BEFORE `init_programs` — every default (materials, lighting,
    // vertex programs, post graphs) resolves disk-first AT COOK TIME. Installing it after meant the cooked
    // programs came from the embedded pack and the shipped assets only governed the frame graphs.
    if (const char* aroot = sandbox_getenv("CRD_ASSETS_DIR"); aroot != nullptr && aroot[0] != 0)
    {
        const bool root_ok = scene_renderer.set_asset_root(aroot);
        CRD_LOG_INFO(g_log_sandbox, "asset root '{}' -> {}", aroot, root_ok ? "installed" : "REJECTED");
    }
    // ⛔⛔ REN-40-C2: the policy installs BEFORE the first sync, because a chain is built the first time a mesh
    // becomes a group and `build_lod_chain` REFUSES a second build on the same resource. And it REFUSES TO RUN
    // when asked for and unavailable, for the `--gpu-cull` reason: an arm that silently measures the no-LOD path
    // is a number that will be quoted as an LOD result.
    if (want_lod)
    {
        const char* const asset = lod_asset != nullptr ? lod_asset : "lod/scene_default.crdlod";
        if (!scene_renderer.set_lod_policy_asset(asset))
        {
            CRD_LOG_ERROR(g_log_sandbox,
                          "--lod needs '{}', which did not install (set CRD_ASSETS_DIR to the repo's assets/ "
                          "directory). Refusing to run rather than measure the NO-LOD path as an LOD result.",
                          asset);
            crd::log::shutdown();
            return 2;
        }
    }
    const bool scene_ready = scene_renderer.init(*raster, rm) && scene_renderer.init_programs(*gpu_context)
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
    // REN-39-D2: the ImGui render half now has a DX12 twin — same class, backend chosen by which ctor runs.
    auto imgui_backend =
        want_dx12 ? std::make_unique<crd::imgui::ImGuiGpuBackend>(*gpu_context, *raster, *surface)
                  : std::make_unique<crd::imgui::ImGuiGpuBackend>(
                        *static_cast<crd::gpu::VulkanGpuContext*>(gpu_context.get()), *surface);
    if (!imgui_backend->valid())
    {
        CRD_LOG_ERROR(g_log_sandbox, "ImGui gpu backend init failed");
        crd::log::shutdown();
        return 1;
    }

    // RET-6 pt 4: the debug-draw overlay (axis triad + the infinite grid) composes over the scene
    const bool draw_ready = crd::draw::init(*gpu_context, *raster);
    if (!draw_ready) { CRD_LOG_WARN(g_log_sandbox, "crd-draw init failed -- continuing without the draw overlay"); }
    crd::draw::RenderBuffer draw_buf(crd::memory::default_allocator());
    if (draw_ready) // the RET-6 debug-draw suite over the real scene depth (wire shapes + the translucent slab)
    {
        crd::draw::axis_triad_to(draw_buf, crd::math::Mat4f::identity(), 3.0F, 5.0F);
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
    bool       lod_reported           = false; // REN-40-C2: the chain report is one-shot, not per frame
    crd::scenerender::SyncStats   last_sync{};
    // REN-8: the sandbox PRESENTS, it never reads pixels back — so skip the per-frame full-target host copy the
    // frame graph does for `read_pixel`. Measured cost of leaving it on: a 7.1 ms stall behind 1.8 ms of passes.
    scene_renderer.set_readback_enabled(force_readback || screenshot_path != nullptr);
    if (want_pull_draws)
    {
        scene_renderer.set_indexed_pull(false);
    } // REN-39-C2: the A/B baseline arm
    // ⭐⭐ REN-40-A: the device-side cull, plus the authored graph that declares its passes. ⛔ Both together —
    // the flag alone would leave the renderer expecting GPU-written commands that no pass ever writes.
    if (want_gpu_cull)
    {
        scene_renderer.set_gpu_cull(true);
        if (want_verify) { scene_renderer.set_gpu_cull_verify(true); }
        // the GRAPH arrives through `post_frames` below — see the note there
        CRD_LOG_INFO(g_log_sandbox, "GPU cull: ON (device-side frustum cull, camera + every cascade){}",
                     want_verify ? " + CPU verify arm" : "");
    }
    if (want_gpu_skin)
    {
        scene_renderer.set_gpu_skinning(true);
        CRD_LOG_INFO(g_log_sandbox, "GPU skin: ON (device-side bone palette)");
    }

    // REN-3.2-b: cascaded shadow maps. `set_shadows_enabled` returns whether they actually became ACTIVE (the
    // cascade shaders had to compile), so a silent "on but not really" is impossible to miss here.
    {
        crd::scenerender::CsmConfig ccfg;
        ccfg.cascade_count = 4;
        ccfg.map_size      = 2048;
        ccfg.far_plane     = 160.0F; // the sandbox field is ~110 units across; cascades past that buy nothing
        scene_renderer.set_csm_config(ccfg);
        // PCSS (contact-hardening): a blocker search sets the filter radius per fragment, so contact points
        // stay sharp and the same shadow softens with caster distance. `--hard-shadows` keeps fixed-radius PCF.
        if (want_pcss)
        {
            scene_renderer.set_soft_shadows(crd::scenerender::SceneRenderer::SoftShadow::Pcss);
        }
        const bool shadows_on = scene_renderer.set_shadows_enabled(want_shadows);
        CRD_LOG_INFO(g_log_sandbox, "REN-3.2-b cascaded shadows: {} ({} cascades @ {}px{})",
                     shadows_on ? "ON" : "unavailable (cascade shaders failed to build)", ccfg.cascade_count,
                     ccfg.map_size, want_pcss ? ", PCSS" : ", PCF");
    }

    // ⛔ HARD RULE: the grid goes through OUR frame graph. `record_overlay_pass` runs as a pass of the scene's
    // graph, so `submit_overlay`'s `draw_overlay` calls hit the raster context's RECORDING path and land in the
    // frame's one command buffer instead of each doing its own submit+wait.
    struct OverlayCtx
    {
        crd::draw::RenderBuffer*          buf      = nullptr;
        crd::gpu::IRasterTarget*          target   = nullptr;
        crd::scenerender::SceneRenderer*  renderer = nullptr; // REN-39: resolves the DECLARED overlay image
        crd::draw::OverlayPassConfig      cfg{};
    } overlay_ctx{&draw_buf, canvas.get(), &scene_renderer, {}};
    scene_renderer.set_overlay_pass(
        [](crd::gpu::IFrameContext& ctx, void* user) {
            auto* o = static_cast<OverlayCtx*>(user);
            // ⭐⭐ REN-39 (the gizmo fix): the overlay draws the image ITS PASS DECLARED — under a post-chain
            // frame that is the scene's HDR transient (live depth, pre-tonemap), not the captured canvas.
            // Drawing the raw canvas here rendered an image the graph never barriered.
            crd::gpu::IRasterTarget* t = o->renderer != nullptr ? o->renderer->overlay_target(ctx) : nullptr;
            if (t == nullptr) { t = o->target; } // no resolvable scene image ⇒ the app's own canvas
            if (!crd::draw::submit_overlay(*t, *o->buf, o->cfg))
            {
                CRD_LOG_WARN(g_log_sandbox, "draw overlay submission refused");
            }
        },
        &overlay_ctx);

    // ── ⭐⭐ 38-G1 IN THE APP: the frame is an ASSET NAME, not text this file carries. Each mode installs a
    // shipped `assets/frame/*.frame.toml` whose post pass names a shipped `assets/post/*.crdp` display
    // transform. Switching the tonemap edits NOTHING here — it selects a different authored frame.
    // ⛔ 38-G1: WITHOUT AN ASSET ROOT the renderer only ever sees the embedded pack — every shipped
    // `assets/**` file is invisible and "edit the asset, see the frame change" is a claim nothing tests.
    // `CRD_ASSETS_DIR` is how ctest points at the tree; the app honours the same variable.
    // ⛔⛔ REN-40-A: the post-mode table IS the frame the run installs, re-installed whenever the tonemap
    // toggles — so a graph installed once at startup is silently REPLACED on the first frame. That is exactly what
    // swallowed the whole device-side cull: it reported "installed", then `forward_csm_agx` (no compute passes)
    // took over and every command stayed at the reset's zero. The switch belongs HERE, in the table the toggle
    // reads, not in a one-shot install racing it.
    const char* const gpu_frame        = (want_gpu_cull || want_gpu_skin) ? "frame/forward_csm_gpu.frame.toml" : nullptr;
    // ⛔⛔ REN-40-B: `--gpu-cull` REFUSES TO RUN WITHOUT ITS GRAPH, and that is a benchmark-integrity rule, not a
    // convenience. `forward_csm_gpu.frame.toml` ships as a FILE, not in the built-in pack, so without
    // `CRD_ASSETS_DIR` the install logs one error line and the run continues — with no cull passes, every
    // indirect command left at the reset's zero, and therefore NOTHING DRAWN. The frame then reports
    // `gpu 0.34 ms` at one million instances, which reads as a spectacular win and is an empty canvas. A
    // performance arm that can silently measure an empty frame will eventually be quoted as a result, so it
    // exits instead.
    if (gpu_frame != nullptr && scene_ready && !scene_renderer.set_frame_graph_asset(gpu_frame))
    {
        CRD_LOG_ERROR(g_log_sandbox,
                      "--gpu-cull/--gpu-skin needs '{}', which did not install (set CRD_ASSETS_DIR to the repo's "
                      "assets/ directory). Refusing to run: without it the device passes do nothing and the frame is EMPTY.",
                      gpu_frame);
        crd::log::shutdown();
        return 2;
    }
    // ⛔ An explicit override wins; failing that the device-cull graph; failing that the tonemap's own frame.
    // ⭐⭐ REN-41: BOTH tonemap arms now carry TAA, and each arm is a DISTINCT frame — 38-G1 makes the display
    // transform a property of the frame, so the radio switches files. Under `--gpu-cull` those files are the two
    // device-cull twins (`forward_csm_gpu_srgb` / `forward_csm_gpu`); otherwise the two CPU-path forward frames.
    // This is why the sRGB radio was inert under `--gpu-cull` before: it mapped BOTH arms to the one gpu frame.
    // Written as a helper rather than nested ternaries so the precedence is a statement, not a parse.
    const auto pick_frame = [&](const char* gpu_variant, const char* def) -> const char* {
        if (frame_override != nullptr) { return frame_override; }
        if (gpu_frame != nullptr) { return gpu_variant; }
        return def;
    };
    const char* const frame_0 = pick_frame("frame/forward_csm_gpu_srgb.frame.toml", "frame/forward_csm_srgb.frame.toml");
    const char* const frame_1 = pick_frame("frame/forward_csm_gpu.frame.toml", "frame/forward_csm_agx.frame.toml");
    const char* const post_frames[2] = {frame_0, frame_1};
    int               post_mode      = 1; // 0 = sRGB only · 1 = AgX
    int               post_mode_live = -1;

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
    // ⭐⭐ REN-41 (TAA): the previous frame's UNJITTERED view_proj + whether it exists. The reproject matrix
    // handed to the renderer is `prev_unjit · inv(cur_jittered)`, so the motion vector carries only real motion.
    crd::math::Mat4f taa_prev_unjit_vp{};
    bool             taa_has_prev = false;
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
            // ⭐⭐⭐ REN-41: the resize recreates the `resizable` taa_history at the new size (its old content is
            // gone). Start TAA FRESH this frame — has_history=false — so the blank buffer is never blended in (no
            // one-frame flash); the history rebuilds cleanly from the next frame.
            taa_has_prev = false;
            if (surface->resize(win_w, win_h))
            {
                canvas = raster->create_color_depth_target(surface->width(), surface->height());
                if (canvas == nullptr) { break; }
            }
        }

        // ⛔ the DETERMINISTIC clock when `--fixed-dt` is given — see the flag's note. `frames` counts presented
        // frames, so the same frame index always carries the same camera pose and the same animation phase.
        const crd::f64 tsec =
            fixed_dt_ms > 0.0
                ? static_cast<crd::f64>(frames_with_present) * (fixed_dt_ms / 1000.0)
                : std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - smoke_start_time).count();

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
        const crd::math::Mat4f proj_unjit = crd::math::perspective_reverse_z(1.0472F, aspect, 0.1F);
        crd::math::Mat4f       proj        = proj_unjit;
        // ── ⭐⭐ REN-41 (TAA): sub-pixel CAMERA JITTER. Each frame the projection is offset by a fraction of a
        // pixel along a Halton(2,3) sequence, so successive frames sample different sub-pixel positions; the TAA
        // resolve accumulates them into a supersampled image. The offset is subpixel, and everything downstream
        // (frustum cull, LOD size, texel-snapped cascades) is invariant to it, so jittering the single vp is safe.
        // ⛔ Only when a TAA frame graph is active (it owns the resolve that removes the jitter) — otherwise the
        // image would visibly shimmer with no accumulator to average it.
        // ⭐⭐ REN-41: TAA is now the DEFAULT for every forward frame the app installs (both tonemap arms, CPU and
        // device-cull), so jitter is on for the whole default path. A custom `--frame` override may name a graph
        // WITHOUT a resolve, so jitter is suppressed there (the override owner opts back in by authoring a resolve).
        const bool taa_on = (frame_override == nullptr);
        if (taa_on)
        {
            const auto halton = [](crd::u32 i, crd::u32 b) {
                float f = 1.0F, r = 0.0F;
                while (i > 0U) { f /= static_cast<float>(b); r += f * static_cast<float>(i % b); i /= b; }
                return r;
            };
            const crd::u32 ji = (frame % 8U) + 1U; // 1..8 (index 0 is the origin — skip it)
            const float    jx = (halton(ji, 2U) - 0.5F) * 2.0F / static_cast<float>(surface->width());
            const float    jy = (halton(ji, 3U) - 0.5F) * 2.0F / static_cast<float>(surface->height());
            proj.c2.x += jx;
            proj.c2.y += jy;
        }
        const crd::math::Mat4f vp   = proj * view;         // JITTERED — raster + cull
        // ⛔⛔ THE REPROJECT MATRIX IS BUILT FROM UNJITTERED PROJECTIONS. Un-jittering the composed `vp` needs the
        // separate view (the jitter rides `proj` then multiplies through `view`), so it is done HERE where both
        // matrices exist, not inside the renderer. `R = prev_unjit · inv(cur_jittered)` maps a current pixel —
        // world reconstructed from the jittered depth — onto last frame's STABLE history grid, so the motion
        // vector carries only real motion and the sub-pixel jitter cancels instead of shimmering.
        if (taa_on && scene_ready)
        {
            const crd::math::Mat4f vp_unjit = proj_unjit * view;
            const crd::math::Mat4f R =
                taa_has_prev ? taa_prev_unjit_vp * crd::math::inverse(vp) : crd::math::Mat4f{};
            scene_renderer.set_taa_reproj(R, taa_has_prev);
            taa_prev_unjit_vp = vp_unjit;
            taa_has_prev      = true;
        }

        const auto t_frame_begin = now_ms();
        // ⛔ BEFORE render(): render() EXECUTES the frame graph, and the overlay runs as a pass inside it. `canvas`
        // is recreated on resize, so refreshing this pointer after the render call leaves the overlay pass using
        // the DESTROYED target for exactly one frame — which is a use-after-free that crashes on a fullscreen
        // toggle. Ordering, not just freshness, is what makes this correct.
        overlay_ctx.target = canvas.get();
        // ⛔ REN-39: the overlay CONFIG must also be current BEFORE render() — the woven overlay pass records
        // INSIDE it. Refreshing the config after the render call fed the pass the PREVIOUS frame's view_proj,
        // so every overlay line lagged the camera by one frame (a ghost beside each line, live only).
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
        // 38-G1: the tonemap is a NAME. Reinstalling the graph is the whole switch.
        if (scene_ready && post_mode != post_mode_live)
        {
            const bool ok = scene_renderer.set_frame_graph_asset(post_frames[post_mode == 1 ? 1 : 0]);
            CRD_LOG_INFO(g_log_sandbox, "38-G1 frame '{}' -> {}", post_frames[post_mode == 1 ? 1 : 0],
                         ok ? "installed" : "REJECTED");
            post_mode_live = post_mode;
        }
        if (scene_ready)
        {
            last_sync         = scene_renderer.sync(world);
            // ⭐⭐ REN-40-C2: say out loud what the chains came out as, ONCE. ⛔ "LOD is on" is a claim; the level
            // count and the triangle counts are the fact, and without them a policy that built NOTHING (a refused
            // decimation, a skinned mesh, a chain that stopped short of its ratio) is indistinguishable from one
            // that worked — on the fps board it just looks like LOD did not help.
            if (scene_renderer.lod_enabled() && !lod_reported)
            {
                lod_reported            = true;
                const auto li           = scene_renderer.lod_chain_info();
                CRD_LOG_INFO(g_log_sandbox,
                             "LOD chains: {}/{} groups have one, max {} levels, {} -> {} tris (level 0 -> coarsest)",
                             li.groups_with_lod, li.groups, li.levels_max, li.tris_level0, li.tris_coarsest);
                // ⛔⛔ THE TABLE ITSELF, PER GROUP. A summary line cannot tell a chain that built from one that
                // published the wrong ranges: "5/7 groups have one" was true while every level past 0 drew
                // NOTHING. The index range and the switch height are what the device actually acts on, so they
                // are what a run has to be able to print.
                crd::u32 gidx = 0U;
                for (const auto& g : scene_renderer.mesh_groups())
                {
                    if (g.lod_count == 0U)
                    {
                        CRD_LOG_INFO(g_log_sandbox, "  group {}: NO CHAIN (draw {} idx)", gidx, g.index_count);
                    }
                    for (crd::u32 l = 0; l < g.lod_count; ++l)
                    {
                        CRD_LOG_INFO(g_log_sandbox, "  group {} lod {}: first_index {} count {} ({} tris) at <{} px",
                                     gidx, l, g.lod_first[l], g.lod_indices[l], g.lod_indices[l] / 3U,
                                     g.lod_height[l]);
                    }
                    ++gidx;
                }
            }
            const auto t_sync = now_ms();
            phase.sync        = ms_between(t_frame_begin, t_sync);
            last_draw = scene_renderer.render(*canvas, vp, crd::math::Vec3f{0.35F, 1.0F, 0.25F},
                                              crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F},
                                              want_no_bvh ? nullptr : bvh);
            phase.render = ms_between(t_sync, now_ms());
            // REN-39: the screenshot arm — dump the PRESENTED canvas (post chain applied) and exit.
            if (screenshot_path != nullptr && tsec >= screenshot_at_s)
            {
                const crd::u32 sw = canvas->width();
                const crd::u32 sh = canvas->height();
                const crd::u32 row = ((sw * 3U + 3U) / 4U) * 4U;
                crd::containers::Array<unsigned char> bmp(crd::memory::default_allocator());
                bmp.resize(54U + static_cast<crd::usize>(row) * sh, static_cast<unsigned char>(0));
                const auto p4 = [&](crd::u32 o, crd::u32 v) {
                    for (crd::u32 k = 0; k < 4U; ++k) { bmp[o + k] = static_cast<unsigned char>((v >> (8U * k)) & 0xFFU); }
                };
                bmp[0] = 'B'; bmp[1] = 'M';
                p4(2U, 54U + row * sh); p4(10U, 54U); p4(14U, 40U); p4(18U, sw); p4(22U, sh);
                bmp[26] = 1U; bmp[28] = 24U; p4(34U, row * sh);
                for (crd::u32 y = 0; y < sh; ++y)
                {
                    for (crd::u32 x = 0; x < sw; ++x)
                    {
                        const crd::u32   px = canvas->read_pixel(x, y); // 0xAABBGGRR
                        const crd::usize o  = 54U + static_cast<crd::usize>(sh - 1U - y) * row + static_cast<crd::usize>(x) * 3U;
                        bmp[o]      = static_cast<unsigned char>((px >> 16U) & 0xFFU); // B
                        bmp[o + 1U] = static_cast<unsigned char>((px >> 8U) & 0xFFU);  // G
                        bmp[o + 2U] = static_cast<unsigned char>(px & 0xFFU);          // R
                    }
                }
                std::FILE* f = nullptr;
#ifdef _WIN32
                (void)fopen_s(&f, screenshot_path, "wb");
#else
                f = std::fopen(screenshot_path, "wb");
#endif
                if (f != nullptr)
                {
                    (void)std::fwrite(bmp.data(), 1U, bmp.size(), f);
                    (void)std::fclose(f);
                    CRD_LOG_INFO(g_log_sandbox, "screenshot -> {} ({}x{})", screenshot_path, sw, sh);
                    // ⭐⭐ REN-40-A: say out loud WHAT THE DEVICE DREW. A GPU cull hides its own count by
                    // construction, so a silently-empty cull and a fast one are indistinguishable from a log that
                    // only prints fps — this prints the per-view survivor counts the indirect commands carry.
                    if (want_gpu_cull)
                    {
                        crd::scenerender::SceneRenderer::GpuCullCounts gc{};
                        if (scene_renderer.read_gpu_cull_counts(gc))
                        {
                            for (crd::u32 v = 0; v < gc.views; ++v)
                            {
                                CRD_LOG_INFO(g_log_sandbox,
                                             "gpu-cull view {}: gpu={} cpu={} {} | index_count={} first_index={} ({} groups)",
                                             v, gc.instances[v], gc.cpu_instances[v],
                                             gc.instances[v] == gc.cpu_instances[v] ? "MATCH" : "*** MISMATCH ***",
                                             gc.indices[v], gc.first_index[v], gc.groups);
                            }
                            // ⭐⭐ REN-40-C2: view 0's commands SLOT BY SLOT — "the cull never chose this level"
                            // and "this level's command is empty" look identical in a per-view total and have
                            // completely different causes.
                            for (crd::u32 sl = 0; sl < 8U; ++sl)
                            {
                                if (gc.slot_instances[sl] == 0U && gc.slot_indices[sl] == 0U) { continue; }
                                CRD_LOG_INFO(g_log_sandbox,
                                             "gpu-cull view0 slot {}: instances={} index_count={} first_index={}",
                                             sl, gc.slot_instances[sl], gc.slot_indices[sl], gc.slot_first[sl]);
                            }
                            CRD_LOG_INFO(g_log_sandbox, "gpu-cull bounds: {}/{} instance AABBs DIFFER on device",
                                         gc.bounds_mismatch, gc.bounds_checked);
                        }
                        else { CRD_LOG_WARN(g_log_sandbox, "gpu-cull: readback unavailable"); }
                    }
                }
                app.close();
            }
            // 38-G1 perf probe: the phase board, once a second, so a headless run can be MEASURED rather
            // than guessed at (the ImGui panel is invisible to a log).
            if (frame % 20U == 0U)
            {
                CRD_LOG_INFO(g_log_sandbox,
                             "perf: sync {:.2f} (extract {:.2f} upload {:.2f} palette {:.2f}) render {:.2f} | "
                             // ⭐⭐ REN-40-B: the extract's COMPLEXITY beside its milliseconds. `chunks` is the
                             // irreducible walk, `re` is how many of them actually moved, `ent` is the per-entity
                             // work that used to be the whole scene every frame.
                             "walk {}c/{}re/{}ent | "
                             "gpu {:.3f} ms ({} passes) cpu {:.3f} | draws {} inst {}{}",
                             phase.sync, last_sync.extract_ms, last_sync.upload_ms, last_sync.palette_ms,
                             phase.render, last_sync.chunks_visited, last_sync.chunks_reextracted,
                             last_sync.entities_extracted, last_draw.gpu_ms, last_draw.timed_passes, last_draw.cpu_ms,
                             last_draw.draws, last_draw.drawn_instances,
                             // ⛔ Under the device cull the CPU never learns the count, so a bare `inst 0` reads
                             // as "nothing was drawn". Say WHY it is 0 — `read_gpu_cull_counts()` is the authority.
                             scene_renderer.gpu_cull() && !scene_renderer.gpu_cull_verify()
                                 ? " (device-driven: the CPU never learns the count)"
                                 : "");
                // 38-G1 perf: the PER-PASS GPU board — "gpu 8.4 ms" is a number, this is an attribution.
                if (const crd::gpu::IFrameGraph* dfg = scene_renderer.debug_frame_graph();
                    dfg != nullptr && dfg->gpu_timing_available())
                {
                    for (crd::u32 pi = 0; pi < dfg->pass_count(); ++pi)
                    {
                        CRD_LOG_INFO(g_log_sandbox, "  pass[{}] {} {:.3f} ms", pi,
                                     dfg->pass_name(pi) != nullptr ? dfg->pass_name(pi) : "?",
                                     dfg->pass_gpu_ms(pi));
                    }
                }
            }
            if (last_draw.draws == 0U) // everything culled (or nothing loadable): still present a cleared frame
            {
                raster->clear(*canvas, crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F});
            }
        }
        else { raster->clear(*canvas, crd::gpu::ClearColor{0.09F, 0.10F, 0.13F, 1.0F}); }

        // (the overlay config refresh moved ABOVE render() — the woven pass records inside it)
        const auto t_after_scene = now_ms();

        const auto t_after_overlay = now_ms();
        phase.overlay              = ms_between(t_after_scene, t_after_overlay);

        imgui_backend->new_frame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        {
            ImGui::Begin("Cerid Sandbox — GEO-7");
            ImGui::Text("frame %u  |  %ux%u", frame, surface->width(), surface->height());
            // ⭐⭐ 38-G1: the DISPLAY TRANSFORM, live. Each radio installs an authored frame whose post pass
            // names a different shipped `.crdp` graph — the pixels change because an ASSET changed.
            ImGui::Separator();
            ImGui::Text("38-G1 tonemap (authored .crdp assets):");
            ImGui::RadioButton("sRGB only", &post_mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("AgX", &post_mode, 1);
            ImGui::Text("  frame asset: %s", post_frames[post_mode == 1 ? 1 : 0]);
            ImGui::Separator();
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

    // Teardown order — two rules, both scars:
    //   ⛔ the SURFACE dies first: its destructor is the vkDeviceWaitIdle that drains the frames in flight.
    //   ⛔ the SCENE RENDERER dies before draw/imgui: its frame graph's descriptor pools hold the LAST frame's
    //     sets, and those sets reference the draw system's buffers — destroying a buffer a live set references
    //     is a validation error (VUID-vkDestroyBuffer-buffer-00922) and a real hazard one driver over.
    // jobs/perf mirror their bring-up.
    crd::jobs::shutdown();
    crd::perf::uninstall_jobs_adapter();
    crd::perf::shutdown();
    surface.reset();            // vkDeviceWaitIdle — every frame in flight completes here
    scene_renderer_ptr.reset(); // the frame graph + its descriptor pools die BEFORE any buffer they referenced
    crd::draw::shutdown();
    imgui_backend.reset();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    canvas.reset();
    raster.reset();
    gpu_context.reset();

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
