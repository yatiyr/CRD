// test_vulkan_hair_swatch_rt.cpp — D-007 B18-f: the PATH-TRACED HAIR SWATCH, on hardware curve traversal.
//
// This is the render the RT strand tier was built for, and it is the one that answers the question the raster path
// could not: what does OUR fibre model actually look like when the visibility is sampled properly?
//
// The scene is the configuration every hair paper uses — strands rooted on a flat patch, no head, no styling. That is
// deliberate: a swatch has nowhere to hide. Everything you see is the BCSDF and the sampling.
//
// ⭐ THE ONE THING THAT MAKES THIS DIFFERENT FROM THE RASTER SHOWCASE. There, ~148 strands overlap each pixel and the
//    deferred buffer keeps ONE (four under 2x2 supersampling); neighbouring pixels keep different winners, which is
//    what produced the streaked, painted look. Here every pixel integrates hundreds of rays against real fibres, so it
//    holds the AVERAGE of the mass. Same BCSDF, same lights, same geometry — the difference is entirely visibility.

#include <crd/gpu/context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/gpu/vulkan_ray_tracing_context.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_hair_rt.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hair_swatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>

namespace gpu = crd::gpu;
namespace kir = crd::kir;
namespace hs  = crd::hairswatch;

namespace
{
using uz_t = crd::usize;
[[nodiscard]] uz_t uz(int v) { return static_cast<uz_t>(v); }

void write_bmp(crd::memory::IAllocator& alloc, const char* path, int w, int h,
               const crd::containers::Array<double>& rgb)
{
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path, "wb") != 0) { f = nullptr; }
#else
    f = std::fopen(path, "wb");
#endif
    if (f == nullptr) { return; }
    const int     rowsz  = ((w * 3 + 3) / 4) * 4;
    const int     imgsz  = rowsz * h;
    const int     filesz = 54 + imgsz;
    unsigned char hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = static_cast<unsigned char>(filesz & 0xFF);
    hdr[3] = static_cast<unsigned char>((filesz >> 8) & 0xFF);
    hdr[4] = static_cast<unsigned char>((filesz >> 16) & 0xFF);
    hdr[5] = static_cast<unsigned char>((filesz >> 24) & 0xFF);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = static_cast<unsigned char>(w & 0xFF);
    hdr[19] = static_cast<unsigned char>((w >> 8) & 0xFF);
    hdr[20] = static_cast<unsigned char>((w >> 16) & 0xFF);
    hdr[22] = static_cast<unsigned char>(h & 0xFF);
    hdr[23] = static_cast<unsigned char>((h >> 8) & 0xFF);
    hdr[24] = static_cast<unsigned char>((h >> 16) & 0xFF);
    hdr[26] = 1; hdr[28] = 24;
    std::fwrite(hdr, 1, 54, f);
    crd::containers::Array<unsigned char> row(&alloc);
    row.resize(uz(rowsz), 0U);
    for (int y = h - 1; y >= 0; --y) // BMP rows run bottom-up
    {
        for (int x = 0; x < w; ++x)
        {
            for (int c = 0; c < 3; ++c)
            {
                const double v = rgb[uz((y * w + x) * 3 + c)];
                const double q = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
                row[uz(x * 3 + (2 - c))] = static_cast<unsigned char>(q * 255.0 + 0.5);
            }
        }
        std::fwrite(row.data(), 1, uz(rowsz), f);
    }
    std::fclose(f);
}

// ACES-ish filmic tonemap. Hair has an enormous dynamic range — the TRT glint on a lit fibre is orders of magnitude
// above the shadowed interior — so a linear clamp either blows the highlights to white or crushes the mass to black.
[[nodiscard]] double tonemap(double x)
{
    const double a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    const double v = (x * (a * x + b)) / (x * (c * x + d) + e);
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}
[[nodiscard]] double srgb(double x) { return x <= 0.0031308 ? 12.92 * x : 1.055 * crd::math::pow(x, 1.0 / 2.4) - 0.055; }

struct Look
{
    const char* name;
    const char* file;
    double      eumelanin;
    double      pheomelanin;
    double      curl_amp;
    double      curl_freq;
    double      wave_amp;
    double      wave_freq;
    double      frizz;
    double      clump_tight;
    double      beta_m;
    double      beta_n;
};
} // namespace

// The swatch render. Tagged [.] so a normal `ctest` run does not spend minutes on it — invoke by name to produce the
// images. It is a RENDER, not an assertion: the gates that certify the maths live in test_ckir_lss / test_vulkan_rt.
TEST_CASE("B18-f showcase: path-traced hair swatch", "[.][gpu-context][vulkan][gpu][rt][hair][showcase]")
{
    crd::memory::TlsfAllocator alloc(3072U << 20U, nullptr, "swatch");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    constexpr int kW      = 1400;
    constexpr int kH      = 1000;
    constexpr int kDebugAov = 0; // 0 = beauty, 1 = the h probe
    constexpr int kOnlyLook = 1; // wavy chestnut — mid-tone shows fibre structure that black hides
    constexpr bool kPerfSweep = false;
    constexpr bool kRealtime  = false; // render the real-time path (1 spp/frame + temporal accumulation)
    constexpr int kPasses   = 32; // × spp per pass. Split so no single dispatch trips the Windows GPU watchdog.

    const Look looks[] = {
        // name              file                        eu     ph    curl  cfreq  wave  wfreq  frizz  clump  bm    bn
        // ⭐ IT IS THE RATIO THAT READS AS A CURL, NOT THE RADIUS. A helix of radius r and pitch p shows, side-on, a
        //   loop p tall and 2r wide — so a ringlet needs 2r comparable to p. At 2r/p ≈ 0.17 the same hair reads as a
        //   crimped spring, which is what the "small radius" pass produced. Against a 0.30 strand: curly wants ~3.4
        //   turns (p ≈ 0.088) at r ≈ 0.030, i.e. 2r/p ≈ 0.68. The earlier wide-helix look was never the radius — it
        //   was the per-strand PHASE, which is now a lock property.
        {"straight black",  "build/sw_straight.bmp",    3.20,  0.10, 0.000, 0.0,  0.006, 0.8,  0.0030, 0.70, 0.22, 0.26},
        {"wavy chestnut",   "build/sw_wavy.bmp",        0.80,  0.55, 0.016, 1.5,  0.012, 0.9,  0.0040, 0.74, 0.25, 0.30},
        {"curly auburn",    "build/sw_curly.bmp",       0.42,  1.35, 0.030, 3.4,  0.008, 1.2,  0.0045, 0.78, 0.27, 0.33},
        {"coily dark",      "build/sw_coily.bmp",       2.10,  0.35, 0.019, 6.5,  0.005, 1.5,  0.0050, 0.80, 0.30, 0.36},
        {"platinum blonde", "build/sw_blonde.bmp",      0.045, 0.09, 0.018, 1.9,  0.011, 1.0,  0.0040, 0.74, 0.24, 0.28},
    };

    crd::containers::Array<float>  segs(&alloc);
    crd::containers::Array<float>  tans(&alloc);
    crd::containers::Array<float>  outf(&alloc);
    crd::containers::Array<double> img(&alloc);
    crd::containers::Array<double> acc(&alloc);
    outf.resize(uz(kW * kH * 3), 0.0F);
    img.resize(uz(kW * kH * 3), 0.0);
    acc.resize(uz(kW * kH * 3), 0.0);

    for (const Look& lk : looks)
    {
        if (&lk != &looks[kOnlyLook]) { continue; } // iterate on ONE look; the rest are a final-pass concern
        hs::SwatchConfig sw;
        sw.curl_amp    = lk.curl_amp;
        sw.curl_freq   = lk.curl_freq;
        sw.wave_amp    = lk.wave_amp;
        sw.wave_freq   = lk.wave_freq;
        sw.stray       = lk.frizz;
        sw.clump_tight = lk.clump_tight;
        // ⛔ SEGMENT COUNT MUST TRACK CURL FREQUENCY. Each swept segment is analytic, so the SURFACE is exact at any
        //    zoom — but the strand's CENTRELINE is still a polyline through these points, and a helix sampled below
        //    about a dozen points per turn renders as a visible zigzag. At 26 segments the 5.5-turn curly swatch came
        //    out as angular wireframe chevrons: not aliasing, an under-tessellated curve. Analytic silhouettes buy
        //    exact thickness, not exact curvature.
        const double turns = lk.curl_freq > lk.wave_freq ? lk.curl_freq : lk.wave_freq;
        //  ~28 points per turn: a helix drawn with 14 reads as a polygon once the swatch fills the frame, and this
        //  camera is much closer than the last one. The centreline is smooth as a function, so this is purely how
        //  finely that smooth curve gets sampled — there is no randomness left for extra points to amplify.
        sw.segments        = static_cast<int>(40.0 > turns * 28.0 ? 40.0 : turns * 28.0);
        // strands PER LOCK carry the density now that the lock count is low; scale it down only where the curl
        // frequency has already inflated the segment count.
        sw.per_clump        = sw.segments > 200 ? 1400 : (sw.segments > 90 ? 1900 : 2600);
        const crd::u32 nseg = hs::build_swatch(sw, segs, tans, &alloc);

        auto scene = rt.build_scene_curves(segs.data(), nseg);
        REQUIRE(scene != nullptr);

        kir::hairrt::RtHairSwatchConfig cfg;
        cfg.width    = kW;
        cfg.height   = kH;
        cfg.spp      = 16;
        cfg.segments = static_cast<int>(nseg);
        cfg.beta_m   = lk.beta_m;
        cfg.beta_n   = lk.beta_n;
        hs::melanin_sigma(lk.eumelanin, lk.pheomelanin, cfg.sigma_a);

        // camera: THREE-QUARTER FROM ABOVE RIGHT. Square-on hides everything a groom is about — from up here you read
        // the strand direction, the depth of the mass, the tips against the plane, and the contact shadow at once.
        // Built from a look-at so the basis stays orthonormal (the kernel only jitters within the pixel).
        {
            const double eye[3] = {0.660, 0.760, 0.720};
            const double tgt[3] = {0.0, 0.380, 0.020};
            double       fwd[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
            const double fl     = crd::math::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
            for (int k = 0; k < 3; ++k) { fwd[k] /= fl; }
            const double wup[3] = {0.0, 1.0, 0.0};
            double       rgt[3] = {fwd[1] * wup[2] - fwd[2] * wup[1], fwd[2] * wup[0] - fwd[0] * wup[2],
                             fwd[0] * wup[1] - fwd[1] * wup[0]};
            const double rl = crd::math::sqrt(rgt[0] * rgt[0] + rgt[1] * rgt[1] + rgt[2] * rgt[2]);
            for (int k = 0; k < 3; ++k) { rgt[k] /= rl; }
            const double upv[3] = {rgt[1] * fwd[2] - rgt[2] * fwd[1], rgt[2] * fwd[0] - rgt[0] * fwd[2],
                                   rgt[0] * fwd[1] - rgt[1] * fwd[0]};
            for (int k = 0; k < 3; ++k)
            {
                cfg.cam_pos[k] = eye[k];
                cfg.cam_fwd[k] = fwd[k];
                cfg.cam_right[k] = rgt[k];
                cfg.cam_up[k] = upv[k];
            }
        }
        cfg.tan_half_fov = 0.115;
        cfg.ground       = true;
        cfg.plane_y      = 0.0;

        // KEY front-left-above · RIM behind (the TT/TRT transmission glow — the light that makes hair look like hair,
        // and the one a raster path with an opacity-map shadow renders least convincingly) · cool FILL.
        const double L0[3] = {-0.52, 0.62, 0.58};
        const double L1[3] = {0.18, 0.32, -0.93};
        const double L2[3] = {0.72, -0.10, 0.68};
        for (int k = 0; k < 3; ++k) { cfg.light_dir[0][k] = L0[k]; cfg.light_dir[1][k] = L1[k]; cfg.light_dir[2][k] = L2[k]; }
        const double C0[3] = {4.20, 4.05, 3.85};
        const double C1[3] = {5.10, 4.70, 4.15};
        const double C2[3] = {0.70, 0.78, 0.98};
        for (int k = 0; k < 3; ++k) { cfg.light_col[0][k] = C0[k]; cfg.light_col[1][k] = C1[k]; cfg.light_col[2][k] = C2[k]; }
        cfg.light_radius[0] = 0.045;
        cfg.light_radius[1] = 0.030;
        cfg.light_radius[2] = 0.150;
        // the jitter frame for each light's disc — any orthonormal pair perpendicular to the direction will do
        for (int L = 0; L < 3; ++L)
        {
            const double d[3] = {cfg.light_dir[L][0], cfg.light_dir[L][1], cfg.light_dir[L][2]};
            const double up[3] = {crd::math::abs(d[1]) > 0.9 ? 1.0 : 0.0, crd::math::abs(d[1]) > 0.9 ? 0.0 : 1.0, 0.0};
            double       t[3]  = {up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0]};
            const double tl = crd::math::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            for (int k = 0; k < 3; ++k) { t[k] /= tl; }
            const double b[3] = {d[1] * t[2] - d[2] * t[1], d[2] * t[0] - d[0] * t[2], d[0] * t[1] - d[1] * t[0]};
            for (int k = 0; k < 3; ++k) { cfg.light_t[L][k] = t[k]; cfg.light_b[L][k] = b[k]; }
        }
        cfg.shadow_tmin = 0.5 * sw.root_radius; // the normal offset does the clearing now, not this
        cfg.ray_tmin    = 0.5 * sw.root_radius;
        cfg.bounces     = 3;
        cfg.debug_aov   = kDebugAov;
        cfg.ray_tmin    = 0.5 * sw.root_radius;
        cfg.bounces     = 3;
        cfg.debug_aov   = kDebugAov; // clear the fibre this ray started on

        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::hairrt::build_rt_hair_swatch_kernel(g, cfg);
        kir::GlslKernel   kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "hair_swatch", &alloc);
        INFO("GLSL: " << spv.error_message.c_str());
        REQUIRE(spv.ok);

        // ── PERF SWEEP. trace_dispatch rebuilds the pipeline + reallocates buffers + blocks on a fence + reads back
        //    the whole image every call, so a single pass folds a large FIXED cost into the GPU work. Sweeping spp in
        //    single dispatches separates them: the SLOPE (Δtime / Δspp) is the pure per-sample GPU cost, the INTERCEPT
        //    is the fixed per-dispatch overhead a real renderer pays ONCE, not per pass.
        if (kPerfSweep)
        {
            // Per-config GPU slope. For each (bounces, shadow_steps) config, time one dispatch at spp=8 and one at
            // spp=96; the SLOPE (t96−t8)/88 is the pure per-sample GPU cost, free of the ~3.35 s fixed per-dispatch
            // overhead. This is what grounds the real-time lever analysis with measured numbers, not estimates.
            struct Cfg { const char* name; int bounces; int shadow; };
            const Cfg cfgs[5] = {
                {"OFFLINE 3-bounce, 8-step shadow", 3, 8},
                {"2-bounce, 4-step shadow",          2, 4},
                {"1-bounce, 2-step shadow",          1, 2},
                {"1-bounce, 1-step shadow (hard)",   1, 1},
                {"1-bounce, 0-step (no shadow)",     1, 0},
            };
            const auto timed = [&](const kir::hairrt::RtHairSwatchConfig& c) {
                kir::KGraph       pg(&alloc);
                const kir::KEntry pe = kir::hairrt::build_rt_hair_swatch_kernel(pg, c);
                kir::GlslKernel   pk(&alloc);
                REQUIRE(kir::emit_compute_kernel_glsl(pg, pe, &alloc, pk));
                const auto psv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(pk.source), "perf", &alloc);
                REQUIRE(psv.ok);
                const float sf = 1.0F;
                gpu::VulkanRayTracingContext::Binding pb[4] = {};
                pb[0].upload = segs.data(); pb[0].bytes = static_cast<crd::u64>(nseg) * 8U * sizeof(float); pb[0].binding = 1U;
                pb[1].upload = &sf; pb[1].bytes = sizeof(float); pb[1].binding = 2U;
                pb[2].readback = outf.data(); pb[2].bytes = static_cast<crd::u64>(kW) * kH * 3U * sizeof(float); pb[2].binding = 3U;
                pb[3].upload = tans.data(); pb[3].bytes = static_cast<crd::u64>(nseg) * 6U * sizeof(float); pb[3].binding = 4U;
                const auto ta = std::chrono::steady_clock::now();
                REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(psv.spirv.data(), psv.spirv.size()),
                                          crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(pb, 4),
                                          static_cast<crd::u32>((kW * kH + 63) / 64)));
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ta).count();
            };
            std::printf("[lever] config                             ms/full-frame-sample  (1400x1000)\n");
            for (const Cfg& c : cfgs)
            {
                kir::hairrt::RtHairSwatchConfig lo = cfg, hi = cfg;
                lo.bounces = c.bounces; lo.shadow_steps = c.shadow; lo.ground_shadow_steps = c.shadow > 0 ? c.shadow : 0; lo.spp = 8;
                hi = lo; hi.spp = 96;
                const double slope = (timed(hi) - timed(lo)) / 88.0;
                std::printf("[lever] %-40s %8.2f ms   (60fps budget buys %.2f spp)\n", c.name, slope, 16.6 / slope);
            }
            continue;
        }

        // ── REAL-TIME MODE. The offline path is a FILM REFERENCE (3 bounces, 8-step shadows, ~512 spp to converge).
        //    Real-time RT is a different recipe: the CHEAP per-frame config (1 bounce + short shadow, the measured
        //    6.7× lever) at 1 spp PER FRAME, with TEMPORAL ACCUMULATION converging the noise across frames. This
        //    demonstrates both halves — the per-frame cost (measured) and that accumulation cleans a 1-spp frame — and
        //    saves frame-1 (what one real-time frame actually looks like) next to the accumulated result.
        if (kRealtime)
        {
            kir::hairrt::RtHairSwatchConfig rc = cfg;
            rc.bounces             = 1;   // the interior GI becomes the dual-scattering ambient below, not a ray path
            rc.shadow_steps        = 2;   // a 2-fibre transmittance shadow — soft + coloured, a fraction of the 8-step march
            rc.ground_shadow_steps = 2;
            rc.env_lo[0] = 0.05; rc.env_lo[1] = 0.052; rc.env_lo[2] = 0.058; // a lifted ambient stands in for the lost bounces
            rc.env_hi[0] = 0.34; rc.env_hi[1] = 0.35;  rc.env_hi[2] = 0.40;
            rc.spp                 = 1;   // ONE sample per frame — the real-time contract
            kir::KGraph       rg(&alloc);
            const kir::KEntry re = kir::hairrt::build_rt_hair_swatch_kernel(rg, rc);
            kir::GlslKernel   rk(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(rg, re, &alloc, rk));
            const auto rsv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(rk.source), "rt_hair", &alloc);
            REQUIRE(rsv.ok);

            const int kFrames = 96;
            for (uz_t i = 0; i < acc.size(); ++i) { acc[i] = 0.0; }
            double frame_ms_sum = 0.0;
            double frame_ms_min = 1.0e30;
            for (int fr = 0; fr < kFrames; ++fr)
            {
                const float seedf = static_cast<float>(fr + 1);
                gpu::VulkanRayTracingContext::Binding rb[4] = {};
                rb[0].upload = segs.data(); rb[0].bytes = static_cast<crd::u64>(nseg) * 8U * sizeof(float); rb[0].binding = 1U;
                rb[1].upload = &seedf; rb[1].bytes = sizeof(float); rb[1].binding = 2U;
                rb[2].readback = outf.data(); rb[2].bytes = static_cast<crd::u64>(kW) * kH * 3U * sizeof(float); rb[2].binding = 3U;
                rb[3].upload = tans.data(); rb[3].bytes = static_cast<crd::u64>(nseg) * 6U * sizeof(float); rb[3].binding = 4U;
                const auto fa = std::chrono::steady_clock::now();
                REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(rsv.spirv.data(), rsv.spirv.size()),
                                          crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(rb, 4),
                                          static_cast<crd::u32>((kW * kH + 63) / 64)));
                const double fms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fa).count();
                // ⚠ this per-frame time INCLUDES the ~3.35 s pipeline-rebuild + readback overhead of the one-shot
                //   trace_dispatch — a harness artifact, NOT a rendering cost. Subtract the measured fixed cost to get
                //   the real per-frame GPU number; the lever sweep above gives the clean per-sample figure.
                if (fr > 0) { frame_ms_sum += fms; frame_ms_min = fms < frame_ms_min ? fms : frame_ms_min; }
                for (uz_t i = 0; i < acc.size(); ++i) { acc[i] += static_cast<double>(outf[i]); }
                if (fr == 0)
                {
                    for (uz_t i = 0; i < img.size(); ++i) { img[i] = srgb(tonemap(static_cast<double>(outf[i]) * 4.0)); }
                    write_bmp(alloc, "build/rt_1frame.bmp", kW, kH, img);
                }
            }
            const double invf = 4.0 / static_cast<double>(kFrames); // 4.0 = the same exposure the 1-frame used
            for (uz_t i = 0; i < img.size(); ++i) { img[i] = srgb(tonemap(acc[i] * invf)); }
            write_bmp(alloc, "build/rt_accum.bmp", kW, kH, img);
            std::printf("[realtime] 1-bounce 2-step shadow, 1 spp/frame @ %dx%d: wall %.1f ms/frame (incl. ~3.35s "
                        "harness overhead) — the lever sweep's 29 ms/spp is the true GPU cost; %d frames accumulated\n",
                        kW, kH, frame_ms_sum / (kFrames - 1), kFrames);
            std::printf("[realtime]   => real GPU frame ~29 ms full-screen 1.4Mpix (34 fps); ~half-res or partial "
                        "coverage clears 60 fps. Wrote build/rt_1frame.bmp (one frame) + build/rt_accum.bmp (accumulated)\n");
            continue;
        }

        for (uz_t i = 0; i < acc.size(); ++i) { acc[i] = 0.0; }
        const auto t_start = std::chrono::steady_clock::now();
        for (int p = 0; p < kPasses; ++p)
        {
            const float seedf = static_cast<float>(p + 1);
            gpu::VulkanRayTracingContext::Binding bind[4] = {};
            bind[0].upload  = segs.data();
            bind[0].bytes   = static_cast<crd::u64>(nseg) * 8U * sizeof(float);
            bind[0].binding = 1U;
            bind[1].upload  = &seedf;
            bind[1].bytes   = sizeof(float);
            bind[1].binding = 2U;
            bind[2].readback = outf.data();
            bind[2].bytes    = static_cast<crd::u64>(kW) * static_cast<crd::u64>(kH) * 3U * sizeof(float);
            bind[2].binding  = 3U;
            bind[3].upload   = tans.data();
            bind[3].bytes    = static_cast<crd::u64>(nseg) * 6U * sizeof(float);
            bind[3].binding  = 4U;
            REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                      crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 4),
                                      static_cast<crd::u32>((kW * kH + 63) / 64)));
            for (uz_t i = 0; i < acc.size(); ++i) { acc[i] += static_cast<double>(outf[i]); }
        }

        const auto   t_end   = std::chrono::steady_clock::now();
        const double sec     = std::chrono::duration<double>(t_end - t_start).count();
        const int    totspp  = kPasses * cfg.spp;
        // Wall-clock across every pass INCLUDING a full-image readback + host accumulate per pass — an upper bound on
        // GPU cost, not a clean device timing. Reported per sample-per-pixel so it scales to any spp / resolution.
        std::printf("[perf] %s: %.2fs for %d spp @ %dx%d (%d strands / %u seg)  =>  %.3f ms/frame-at-1spp, %.1f Mrays/s\n",
                    lk.name, sec, totspp, kW, kH, sw.clumps() * sw.per_clump, nseg,
                    1000.0 * sec / static_cast<double>(totspp),
                    static_cast<double>(kW) * kH * totspp / sec / 1.0e6);

        const double inv = 1.0 / static_cast<double>(kPasses * cfg.spp);

        // ── THE PROBE READ-OUT. R holds sum|h| over HITS ONLY and G the hit count, so R/G is the mean |h| a ray sees
        //    GIVEN that it hit a fibre — the quantity theory predicts, free of any coverage blend. Reported only over
        //    pixels the fibres actually cover well, since a 2%-covered pixel estimates its mean from ~10 samples.
        if (cfg.debug_aov != 0)
        {
            int    hist[10] = {};
            int    n        = 0;
            double sum      = 0.0;
            for (uz_t i = 0; i < img.size(); i += 3U)
            {
                const double hits = acc[i + 1U];
                if (hits < 0.25 * static_cast<double>(kPasses * cfg.spp)) { continue; } // ≥25% coverage
                const double mh = acc[i] / hits;
                int          b  = static_cast<int>(mh * 10.0);
                if (b < 0) { b = 0; }
                if (b > 9) { b = 9; }
                ++hist[b];
                sum += mh;
                ++n;
            }
            std::printf("[h probe] %d well-covered pixels, mean|h| = %.4f   (theory for a cylinder: 0.5)\n", n,
                        n > 0 ? sum / static_cast<double>(n) : 0.0);
            for (int b = 0; b < 10; ++b)
            {
                std::printf("    |h| %.1f-%.1f : %7d\n", 0.1 * b, 0.1 * (b + 1), hist[b]);
            }
            continue;
        }

        // ── AUTO-EXPOSURE (Reinhard's key). Absorption spans two orders of magnitude between platinum and black, so a
        //    fixed exposure cannot serve both: the same setting that holds detail in blonde crushes black hair to a
        //    silhouette, and the one that opens up black blows blonde to flat paper. Every film shot is graded; grade
        //    each swatch to the same middle-grey key so what differs between them is the SCATTERING, not the printing.
        //    Keyed off the LOG-average of pixels that actually hit hair — the background would otherwise drag it down.
        constexpr double kKey = 0.20;
        double           logsum = 0.0;
        int              nhair  = 0;
        double           peak   = 0.0;
        for (uz_t i = 0; i < img.size(); i += 3U)
        {
            const double r = acc[i] * inv, gq = acc[i + 1U] * inv, b = acc[i + 2U] * inv;
            const double lum = 0.2126 * r + 0.7152 * gq + 0.0722 * b;
            if (lum > peak) { peak = lum; }
            if (lum > cfg.bg[1] * 1.5) { logsum += crd::math::log(lum + 1.0e-6); ++nhair; }
        }
        const double key   = nhair > 0 ? crd::math::exp(logsum / static_cast<double>(nhair)) : 1.0;
        const double scale = kKey / (key > 1.0e-6 ? key : 1.0e-6);
        double       mean  = 0.0;
        for (uz_t i = 0; i < img.size(); ++i)
        {
            const double v = acc[i] * inv * scale;
            mean += v;
            img[i] = srgb(tonemap(v));
        }
        mean /= static_cast<double>(img.size());
        std::printf("[swatch] %-16s %d strands / %u segments  %dx%d @ %d spp   peak=%.3f mean=%.4f -> %s\n", lk.name,
                    sw.clumps() * sw.per_clump, nseg, kW, kH, kPasses * cfg.spp, peak, mean, lk.file);
        write_bmp(alloc, lk.file, kW, kH, img);
    }
}
