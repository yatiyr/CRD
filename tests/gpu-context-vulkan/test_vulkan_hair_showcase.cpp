// test_vulkan_hair_showcase.cpp — D-007 B18: the hair SHOWCASE, rendered with the kernels running ON THE GPU.
//
// Identical renderer to the regression gate (tests/kir/hair_render.hpp) — same groom kernel, same Chiang BCSDF kernel,
// same deep-opacity-map kernels, same compositing filter. The ONLY difference is the dispatch hook: the gate leaves it
// null and gets the CPU oracle, this file points it at Vulkan and gets the device.
//
// ⭐ WHY THIS FILE LIVES HERE AND NOT IN tests/kir. The KIR test target deliberately does NOT link Vulkan — that is the
//   link-isolation smoke (ADR-0098), which proves the IR and its CPU reference drag no GPU API into a consumer. So a
//   GPU-backed render simply cannot live in that target. It belongs in the backend target that already links Vulkan.
//
// WHAT IT DEMONSTRATES. Hair TYPE is almost entirely the helical styling operator from the B18-d strand kernel fighting
// gravity: a coil has high curl amplitude and frequency and resists droop; straight hair has neither and falls. Hair
// COLOUR is the Chiang 2016 melanin pair — eumelanin (brown/black) against pheomelanin (red/yellow). Pigment lives in the
// TRANSMISSIVE lobes, so it only shows where light passes THROUGH the fibre; that is why every variant keeps a strong
// warm rim light, and why an RGB tint applied to a diffuse term never looks like hair.

#include "../kir/hair_render.hpp"
#include "../gpu-shared/ckir_kernel_dispatch.hpp"

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/kir/ckir_glsl.hpp>

#include <crd/containers/string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace hr  = hair_render;
namespace gpu = crd::gpu;

namespace
{
// The dispatch hook's context: a live compute context plus a SPIR-V cache. Compiling the same graph once per light pass
// per channel would otherwise dominate the frame — the shader compile is far more expensive than the dispatch.
struct GpuDispatcher
{
    crd::gpu::VulkanComputeContext* compute = nullptr;
    crd::memory::IAllocator*        alloc   = nullptr;
    int                             kernels = 0;
    int                             compiles = 0;

    // cache keyed by the emitted GLSL source (cheap and exact: identical source ⇒ identical pipeline)
    struct Entry
    {
        crd::containers::String            src;
        std::unique_ptr<gpu::ComputePipeline> pipe;
        explicit Entry(crd::memory::IAllocator* a) : src(a) {}
    };
    crd::containers::Array<std::unique_ptr<Entry>> cache;
    explicit GpuDispatcher(crd::memory::IAllocator* a) : alloc(a), cache(a) {}
};

void gpu_dispatch(void* ctx, crd::kir::KGraph& g, const crd::kir::KEntry& e, crd::kir::KernelBuffer* bufs, int nbufs,
                  crd::u32 groups)
{
    namespace kir = crd::kir;
    auto* d       = static_cast<GpuDispatcher*>(ctx);
    ++d->kernels;

    kir::GlslKernel kern(d->alloc);
    if (!kir::emit_compute_kernel_glsl(g, e, d->alloc, kern)) { return; }

    gpu::ComputePipeline* pipe = nullptr;
    for (crd::usize i = 0; i < d->cache.size(); ++i)
    {
        if (d->cache[i]->src == kern.source) { pipe = d->cache[i]->pipe.get(); break; }
    }
    if (pipe == nullptr)
    {
        ++d->compiles;
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "hair_showcase", d->alloc, false);
        if (!spv.ok) { std::printf("  [gpu] GLSL compile FAILED: %s\n", spv.error_message.c_str()); return; }
        auto en = std::make_unique<GpuDispatcher::Entry>(d->alloc);
        en->src = kern.source;
        // n_bindings is an int in the factory signature — casting to u32 here only narrows straight back.
        en->pipe = d->compute->create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbufs, 0U);
        if (en->pipe == nullptr) { return; }
        pipe = en->pipe.get();
        d->cache.push_back(std::move(en));
    }

    // The device harness is F32 (a kernel with F64 storage needs shaderFloat64, which is not universal) — so convert
    // in, dispatch, convert back. That single-precision round trip is exactly what the B18 GPU gates measure against
    // the oracle, and it lands at ~1e-6, far below anything visible in an 8-bit image.
    int total = 0;
    for (int b = 0; b < nbufs; ++b) { total += bufs[b].len; }
    crd::containers::Array<float> store(d->alloc);
    store.resize(static_cast<crd::usize>(total), 0.0F);
    float* host[8] = {};
    int    lens[8] = {};
    int    off     = 0;
    for (int b = 0; b < nbufs; ++b)
    {
        host[b] = store.data() + off;
        lens[b] = static_cast<int>(bufs[b].len);
        for (int i = 0; i < bufs[b].len; ++i) { host[b][i] = static_cast<float>(bufs[b].data[i]); }
        off += bufs[b].len;
    }
    crd::kir_test::dispatch_kernel_1wg(*d->compute, *pipe, host, lens, nbufs, groups);
    for (int b = 0; b < nbufs; ++b)
    {
        for (int i = 0; i < bufs[b].len; ++i) { bufs[b].data[i] = static_cast<double>(host[b][i]); }
    }
}

// ── The four canonical hair types, expressed as styling-versus-gravity. Note how little else changes. ────────────────
[[nodiscard]] hr::HairLook groom_straight()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.004; l.curl_amp_hi = 0.012; // essentially no helix
    l.curl_freq_lo = 0.7;  l.curl_freq_hi = 1.1;
    l.taper = 0.15;
    l.droop = 3.05;    // nothing resists gravity ⇒ it hangs long and heavy
    l.out_lift = 0.80; // lies close to the scalp: straight hair has little volume
    l.beta_m = 0.125;  // smoother cuticle ⇒ a tighter, glassier primary highlight
    l.beta_n = 0.27;
    return l;
}
[[nodiscard]] hr::HairLook groom_wavy()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.030; l.curl_amp_hi = 0.075;
    l.curl_freq_lo = 1.2;  l.curl_freq_hi = 2.6;
    l.taper = 0.35;
    l.droop = 2.55;
    l.out_lift = 1.00;
    return l;
}
[[nodiscard]] hr::HairLook groom_curly()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.085; l.curl_amp_hi = 0.150;
    l.curl_freq_lo = 3.4;  l.curl_freq_hi = 5.2;
    l.taper = 0.70;    // the curl HOLDS to the tip instead of relaxing
    l.droop = 1.85;    // the coil's spring partly beats gravity
    l.out_lift = 1.35; // ...which is what gives curly hair its volume
    l.beta_m = 0.175;  // rougher cuticle ⇒ the highlight breaks into glints instead of one band
    l.beta_n = 0.36;
    l.len_jitter = 0.62;
    return l;
}
[[nodiscard]] hr::HairLook groom_coily()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.140; l.curl_amp_hi = 0.215;
    l.curl_freq_lo = 6.5;  l.curl_freq_hi = 9.5;
    l.taper = 0.92;
    l.droop = 1.05;    // a tight coil barely droops at all
    l.out_lift = 1.70; // it stands OFF the scalp — the silhouette is the giveaway
    l.beta_m = 0.195;
    l.beta_n = 0.40;
    l.len_jitter = 0.70;
    return l;
}

// Natural hair colour is a 2-D melanin space, not an RGB picker. These are its canonical corners.
struct Pigment
{
    const char* name;
    double      eu; // eumelanin   — brown/black
    double      ph; // pheomelanin — red/yellow
    double      exposure;
};
const Pigment kPigments[5] = {
    {"black",    8.00, 0.00, 1.70}, // near-total absorption ⇒ the whole look is the R lobe (surface specular)
    {"brown",    1.30, 0.20, 1.15},
    {"auburn",   0.30, 1.50, 1.05}, // pheomelanin-dominant: TT/TRT carry a red that only appears lit from behind
    {"blonde",   0.10, 0.20, 0.95},
    {"platinum", 0.02, 0.05, 0.72}, // almost no pigment ⇒ multiple scattering dominates; needs the least exposure
};

[[nodiscard]] hr::SceneConfig showcase_scene()
{
    hr::SceneConfig sc;
    // FULL RESOLUTION. At 760x900 a strand is ~1 px and the filter's reach is 5 px, so the reconstruction dominates the
    // signal - detail is destroyed before it can be seen. Quadrupling the pixel count makes a fibre resolvable and lets
    // the filter shrink to its real job (anti-aliasing) instead of painting.
    sc.width  = 1000;
    sc.height = 1250;
    // 2x2 = FOUR independent strand samples per output pixel. This is the single biggest quality lever left: without it
    // every pixel commits to one of ~148 overlapping strands and the groom reads as line art rather than as hair.
    sc.supersample = 2;
    // PRODUCTION DENSITY. 1800 tufts x 96 = 172,800 render strands - the range a shipped character groom actually uses.
    // This is affordable now only because the whole groom generates in ONE dispatch with per-strand styling from buffers.
    sc.bundles      = 1800;
    sc.per_bundle   = 96;
    sc.bundle_width = 0.055;
    sc.points       = 28;   // a tight coil needs samples or its helix reads as a polyline
    sc.hair_radius  = 0.0011; // thinner fibres: more of them, each contributing partial coverage rather than occluding
    sc.lmap         = 384;  // the self-shadow map has to grow with strand count or its cells saturate and go black
    sc.lfrag        = 64;
    sc.dom_alpha    = 0.042; // ~2.7x the fragments per cell vs the last pass, so ~2.7x less opacity each
    sc.ms_gain      = 0.075; // more inter-fibre bounce: the body was reading as a dark shell with a lit rim
    // The filter now only anti-aliases; at this density and resolution there are no gaps left to bridge.
    sc.filter_sigma_par  = 1.35;
    sc.filter_sigma_perp = 0.62;
    sc.filter_radius     = 3;
    // FRAMING. The groom was occupying about a third of the image, which throws away most of the resolution. Move in and
    // widen the lens so it fills roughly three quarters of the frame height.
    sc.eye        = {0.80, -4.20, 0.50};
    sc.at         = {0.0, 0.0, -0.62};
    sc.flen       = 1.45;
    // The shadow side was reading black. Raise the fill and cool it (bounce light is never the key's colour), and lift the
    // key so its highlight band sits ON the crown rather than skimming past it.
    sc.key_dir    = {-0.55, -0.62, 0.56};
    sc.key_int    = 3.10;
    sc.fill_dir   = {0.86, -0.30, -0.12};
    sc.fill_col   = {0.46, 0.55, 0.78};
    sc.fill_int   = 0.95;
    sc.rim_int    = 7.20;
    // ⚠ OFF until the COMBINATION is right. The B18-c tier is fully wired and dispatching (hair_render.hpp), and the
    //   kernels themselves are gated and GPU-verified. What is wrong is how this renderer combines them: Ψ^G carries a
    //   directional SPREAD term S_f — a Gaussian in (θd + θi) of width n·β̄f² — and applying that as a scalar multiplier
    //   on the direct lobe zeroes everything off the specular cone, rendering the groom black. T_f is the scalar
    //   attenuation; S_f describes the spread of the SCATTERED component only. Measured fibre counts: n ≈ 0.3 for the
    //   key, ≈ 9.7 for the rim (which sits behind the groom) — so the rim, the dominant light, collapses first.
    //   Getting that combination right is the remaining B18-e work. Until then the placeholder below is used, and it is
    //   labelled as a placeholder rather than passed off as the real tier.
    sc.dual_scatter = true;
    sc.verbose    = false;
    return sc;
}
} // namespace

TEST_CASE("B18 SHOWCASE: hair types and colours rendered on the GPU", "[.showcase][gpu-context][vulkan][gpu][hair]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    // ⛔ 6144U << 20U OVERFLOWS u32 and silently yields 2 GiB, not 6 - the shift is evaluated in unsigned int before it
    //   ever reaches the usize parameter. A supersampled frame then dies in the allocator with no hint of why.
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(3584) << 20U); // TLSF tops out at 4 GB (kFlIndexMax = 32)
    GpuDispatcher              disp(&alloc);
    disp.compute = &compute;

    hr::SceneConfig sc  = showcase_scene();
    sc.dispatch_ctx     = &disp;
    sc.dispatch         = &gpu_dispatch;

    struct Shape
    {
        const char*  file;
        hr::HairLook look;
    };
    const Shape shapes[4] = {{"build/showcase_straight.bmp", groom_straight()},
                             {"build/showcase_wavy.bmp", groom_wavy()},
                             {"build/showcase_curly.bmp", groom_curly()},
                             {"build/showcase_coily.bmp", groom_coily()}};

    // ── SET 1: four hair TYPES at one pigment, so the comparison isolates SHAPE ──
    for (const Shape& sh : shapes)
    {
        hr::HairLook l = sh.look;
        l.eumelanin    = 1.30;
        l.pheomelanin  = 0.20;
        sc.exposure    = 1.15;
        crd::containers::Array<double> img(&alloc);
        const hr::Stats                st = hr::render(alloc, sc, l, img);
        hr::write_bmp(sh.file, sc.width, sc.height, img);
        std::printf("[showcase] %-30s covered=%6d partial=%5d (%2.0f%%) mean=%.3f  [%d kernels, %d compiled]\n", sh.file,
                    st.covered, st.partial,
                    st.covered > 0 ? 100.0 * static_cast<double>(st.partial) / static_cast<double>(st.covered) : 0.0,
                    st.mean, disp.kernels, disp.compiles);
        CHECK(st.covered > 0);
        CHECK(st.mean > 0.02);
    }

    // ── SET 2: five PIGMENTS on one groom, so the comparison isolates COLOUR ──
    for (const Pigment& pg : kPigments)
    {
        hr::HairLook l = groom_wavy();
        l.eumelanin    = pg.eu;
        l.pheomelanin  = pg.ph;
        sc.exposure    = pg.exposure;
        crd::containers::Array<double> img(&alloc);
        const hr::Stats                st = hr::render(alloc, sc, l, img);
        char                           path[128];
        std::snprintf(path, sizeof(path), "build/showcase_%s.bmp", pg.name);
        hr::write_bmp(path, sc.width, sc.height, img);
        std::printf("[showcase] %-30s eu=%.2f ph=%.2f covered=%6d mean=%.3f\n", path, pg.eu, pg.ph, st.covered, st.mean);
        CHECK(st.covered > 0);
        CHECK(st.mean > 0.02);
    }
}
