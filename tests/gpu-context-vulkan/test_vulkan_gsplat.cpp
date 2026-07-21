// test_vulkan_gsplat.cpp — D-007 B19-a: the 3D GAUSSIAN SPLATTING forward rasteriser ON THE GPU.
//
// Proves the primitive renders on-device: build a scene of colour Gaussians, PROJECT them (GPU), depth-SORT (host —
// B19-a2 wires the GPU radix sort), and COMPOSITE front-to-back (GPU). The CKIR maths is already certified against
// closed-form geometry on the CPU oracle (test_ckir_gsplat.cpp); this shows the same kernels lower to GLSL and run.
//
// Scene: a shell of Gaussians on a sphere, each coloured by its surface direction — a "rainbow sphere", the classic
// 3DGS sanity scene. It exercises projection (anisotropy → elliptical footprints), SH degree-0 colour, and the
// depth-sorted over-composite all at once.

#include <crd/gpu/context.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_gsplat.hpp>
#include <crd/kir/ckir_scan.hpp>
#include <crd/kir/ckir_sort.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ckir_kernel_dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace gpu = crd::gpu;
namespace kir = crd::kir;

namespace
{
using uz_t = crd::usize;
[[nodiscard]] uz_t uz(int v) { return static_cast<uz_t>(v); }

void write_bmp(crd::memory::IAllocator& alloc, const char* path, int w, int h, const crd::containers::Array<double>& rgb)
{
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path, "wb") != 0) { f = nullptr; }
#else
    f = std::fopen(path, "wb");
#endif
    if (f == nullptr) { return; }
    const int     rowsz  = ((w * 3 + 3) / 4) * 4;
    const int     filesz = 54 + rowsz * h;
    unsigned char hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = static_cast<unsigned char>(filesz & 0xFF);
    hdr[3] = static_cast<unsigned char>((filesz >> 8) & 0xFF);
    hdr[4] = static_cast<unsigned char>((filesz >> 16) & 0xFF);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = static_cast<unsigned char>(w & 0xFF); hdr[19] = static_cast<unsigned char>((w >> 8) & 0xFF);
    hdr[22] = static_cast<unsigned char>(h & 0xFF); hdr[23] = static_cast<unsigned char>((h >> 8) & 0xFF);
    hdr[26] = 1; hdr[28] = 24;
    std::fwrite(hdr, 1, 54, f);
    crd::containers::Array<unsigned char> row(&alloc);
    row.resize(uz(rowsz), 0U);
    for (int y = h - 1; y >= 0; --y)
    {
        for (int x = 0; x < w; ++x)
        {
            for (int c = 0; c < 3; ++c)
            {
                double v = rgb[uz((y * w + x) * 3 + c)];
                if (v < 0.0) { v = 0.0; }
                if (v > 1.0) { v = 1.0; }
                row[uz(x * 3 + (2 - c))] = static_cast<unsigned char>(crd::math::lround(v * 255.0));
            }
        }
        std::fwrite(row.data(), 1, uz(rowsz), f);
    }
    std::fclose(f);
}
} // namespace

TEST_CASE("B19-a showcase: 3D Gaussian splatting forward render on Vulkan", "[.][gpu-context][vulkan][gpu][gsplat][showcase]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U, nullptr, "gsplat-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr int imw = 512;
    constexpr int imh = 512;

    // ── scene: a sphere shell of Gaussians, coloured by surface direction (a rainbow sphere) ──
    constexpr int rings = 40;
    constexpr int n_seg = 80;
    constexpr int ng    = rings * n_seg;
    const double  rad   = 1.4;
    const double  c0    = kir::gsplat::detail::kShC0;
    crd::containers::Array<float> gauss(&alloc);
    gauss.resize(uz(ng) * 14U, 0.0F);
    int gi = 0;
    for (int r = 0; r < rings; ++r)
    {
        const double theta = 3.14159265358979 * (static_cast<double>(r) + 0.5) / static_cast<double>(rings); // 0..π
        for (int seg = 0; seg < n_seg; ++seg)
        {
            const double phi = 6.28318530717959 * static_cast<double>(seg) / static_cast<double>(n_seg);
            const double nx = crd::math::sin(theta) * crd::math::cos(phi);
            const double ny = crd::math::cos(theta);
            const double nz = crd::math::sin(theta) * crd::math::sin(phi);
            float*       q  = gauss.data() + uz(gi) * 14U;
            q[0] = static_cast<float>(rad * nx); q[1] = static_cast<float>(rad * ny); q[2] = static_cast<float>(rad * nz);
            q[3] = q[4] = q[5] = 0.035F;                          // small isotropic splats
            q[6] = 0.0F; q[7] = 0.0F; q[8] = 0.0F; q[9] = 1.0F;    // identity rotation
            q[10] = 0.9F;                                          // opacity
            // colour from direction (a smooth rainbow), converted to SH degree-0: sh = (colour − 0.5)/C0
            const double cr = 0.5 + 0.5 * nx;
            const double cg = 0.5 + 0.5 * ny;
            const double cb = 0.5 + 0.5 * nz;
            q[11] = static_cast<float>((cr - 0.5) / c0);
            q[12] = static_cast<float>((cg - 0.5) / c0);
            q[13] = static_cast<float>((cb - 0.5) / c0);
            ++gi;
        }
    }

    // ── camera: at (0,0,-4.2) looking toward +z (view-z convention). R = identity, t = (0,0,4.2). ──
    crd::containers::Array<float> cam(&alloc);
    cam.resize(20U, 0.0F);
    cam[0] = 1.0F; cam[4] = 1.0F; cam[8] = 1.0F;
    cam[9] = 0.0F; cam[10] = 0.0F; cam[11] = 4.2F;
    cam[12] = 700.0F; cam[13] = 700.0F;                            // fx, fy
    cam[14] = static_cast<float>(imw) * 0.5F; cam[15] = static_cast<float>(imh) * 0.5F;
    cam[16] = 0.2F; cam[17] = static_cast<float>(imw); cam[18] = static_cast<float>(imh);

    // ── PROJECT on the GPU ──
    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    kir::GlslKernel                  pk(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(pg, pe, &alloc, pk));
    const auto psv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(pk.source), "gsplat_project", &alloc);
    INFO("project GLSL: " << psv.error_message.c_str());
    REQUIRE(psv.ok);
    auto ppipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(psv.spirv.data(), psv.spirv.size()), 3, 0U);
    REQUIRE(ppipe != nullptr);

    crd::containers::Array<float> proj(&alloc);
    proj.resize(uz(ng) * 12U, 0.0F);
    {
        float*    hb[3] = {gauss.data(), cam.data(), proj.data()};
        const int ln[3] = {ng * 14, 20, ng * 12};
        crd::kir_test::dispatch_kernel_1wg(compute, *ppipe, hb, ln, 3, static_cast<crd::u32>((ng + 63) / 64));
    }

    // ── depth SORT (host, nearest-first) — B19-a2 replaces with the GPU radix sort ──
    crd::containers::Array<crd::u32> order(&alloc);
    order.resize(uz(ng), 0U);
    for (int i = 0; i < ng; ++i) { order[uz(i)] = static_cast<crd::u32>(i); }
    // simple insertion-ish sort by depth (slot 2); ng is a few thousand — fine for the showcase
    for (int i = 1; i < ng; ++i)
    {
        const crd::u32 key = order[uz(i)];
        const float    kd  = proj[uz(static_cast<int>(key)) * 12U + 2U];
        int            j   = i - 1;
        while (j >= 0 && proj[uz(static_cast<int>(order[uz(j)])) * 12U + 2U] > kd) { order[uz(j + 1)] = order[uz(j)]; --j; }
        order[uz(j + 1)] = key;
    }
    crd::containers::Array<float> sorted(&alloc);
    sorted.resize(uz(ng) * 12U, 0.0F);
    for (int i = 0; i < ng; ++i)
    {
        const int src = static_cast<int>(order[uz(i)]);
        for (int k = 0; k < 12; ++k) { sorted[uz(i) * 12U + uz(k)] = proj[uz(src) * 12U + uz(k)]; }
    }

    // ── RENDER on the GPU ──
    kir::gsplat::GsplatRenderConfig rcfg;
    rcfg.width = imw; rcfg.height = imh; rcfg.max_splats = ng;
    kir::KGraph       rg(&alloc);
    const kir::KEntry re = kir::gsplat::build_gsplat_render_kernel(rg, rcfg);
    kir::GlslKernel   rk(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(rg, re, &alloc, rk));
    const auto rsv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(rk.source), "gsplat_render", &alloc);
    INFO("render GLSL: " << rsv.error_message.c_str());
    REQUIRE(rsv.ok);
    auto rpipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(rsv.spirv.data(), rsv.spirv.size()), 3, 0U);
    REQUIRE(rpipe != nullptr);

    crd::containers::Array<float> par(&alloc);
    par.resize(8U, 0.0F);
    par[0] = static_cast<float>(ng); par[1] = static_cast<float>(imw); par[2] = static_cast<float>(imh);
    par[3] = 0.03F; par[4] = 0.035F; par[5] = 0.05F; // dim background
    par[6] = 1.0F / 255.0F;
    crd::containers::Array<float> imgf(&alloc);
    imgf.resize(uz(imw * imh) * 4U, 0.0F);
    {
        float*    hb[3] = {sorted.data(), par.data(), imgf.data()};
        const int ln[3] = {ng * 12, 8, imw * imh * 4};
        crd::kir_test::dispatch_kernel_1wg(compute, *rpipe, hb, ln, 3, static_cast<crd::u32>((imw * imh + 63) / 64));
    }

    // ── write the image + a sanity check ──
    crd::containers::Array<double> img(&alloc);
    img.resize(uz(imw * imh) * 3U, 0.0);
    double centre_lum = 0.0;
    for (int p = 0; p < imw * imh; ++p)
    {
        for (int c = 0; c < 3; ++c) { img[uz(p) * 3U + uz(c)] = static_cast<double>(imgf[uz(p) * 4U + uz(c)]); }
    }
    // the sphere fills the centre — a block there must be brighter than the background corner
    for (int y = imh / 2 - 20; y < imh / 2 + 20; ++y)
    {
        for (int x = imw / 2 - 20; x < imw / 2 + 20; ++x)
        {
            centre_lum += img[uz(y * imw + x) * 3U + 1U];
        }
    }
    centre_lum /= (40.0 * 40.0);
    const double corner_lum = img[uz(2 * imw + 2) * 3U + 1U];
    std::printf("[B19-a] rainbow sphere: %d Gaussians, %dx%d — centre lum %.3f vs corner %.3f\n", ng, imw, imh, centre_lum, corner_lum);
    write_bmp(alloc, "build/gsplat_sphere.bmp", imw, imh, img);

    CHECK(centre_lum > 0.2);              // the sphere renders where it should
    CHECK(centre_lum > corner_lum + 0.1); // ...and stands clear of the background
    CHECK(corner_lum < 0.1);              // background clean (radius cull works at scale)

    // ── B19-a2: the TILED render must produce the SAME image on the GPU (the perf structure, portable) ──
    constexpr int tile_px = 16;
    constexpr int tiles_x = (imw + tile_px - 1) / tile_px;
    constexpr int n_tiles = tiles_x * tiles_x;
    constexpr int cap    = 512;
    crd::containers::Array<float> buckets(&alloc);
    crd::containers::Array<float> counts(&alloc);
    buckets.resize(uz(n_tiles) * cap * 12U, 0.0F);
    counts.resize(uz(n_tiles), 0.0F);
    for (int i = 0; i < ng; ++i) // append each depth-sorted splat to every tile its bbox covers (host — B19-a3 on GPU)
    {
        const double mnx   = sorted[uz(i) * 12U + 0U];
        const double mny   = sorted[uz(i) * 12U + 1U];
        const double srad  = sorted[uz(i) * 12U + 6U];
        const double valid = sorted[uz(i) * 12U + 11U];
        if (valid < 0.5) { continue; }
        int tx0 = static_cast<int>(crd::math::floor((mnx - srad) / tile_px));
        int tx1 = static_cast<int>(crd::math::floor((mnx + srad) / tile_px));
        int ty0 = static_cast<int>(crd::math::floor((mny - srad) / tile_px));
        int ty1 = static_cast<int>(crd::math::floor((mny + srad) / tile_px));
        if (tx0 < 0) { tx0 = 0; }  if (ty0 < 0) { ty0 = 0; }
        if (tx1 > tiles_x - 1) { tx1 = tiles_x - 1; }  if (ty1 > tiles_x - 1) { ty1 = tiles_x - 1; }
        for (int ty = ty0; ty <= ty1; ++ty)
        {
            for (int tx = tx0; tx <= tx1; ++tx)
            {
                const int t = ty * tiles_x + tx;
                const int c = static_cast<int>(counts[uz(t)]);
                if (c >= cap) { continue; }
                for (int k = 0; k < 12; ++k) { buckets[(uz(t) * cap + uz(c)) * 12U + uz(k)] = sorted[uz(i) * 12U + uz(k)]; }
                counts[uz(t)] = static_cast<float>(c + 1);
            }
        }
    }

    kir::gsplat::GsplatTiledConfig tcfg;
    tcfg.width = imw; tcfg.height = imh; tcfg.tile_px = tile_px; tcfg.cap = cap;
    kir::KGraph       tg(&alloc);
    const kir::KEntry te = kir::gsplat::build_gsplat_tiled_render_kernel(tg, tcfg);
    kir::GlslKernel   tk(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(tg, te, &alloc, tk));
    const auto tsv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(tk.source), "gsplat_tiled", &alloc);
    INFO("tiled GLSL: " << tsv.error_message.c_str());
    REQUIRE(tsv.ok);
    auto tpipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(tsv.spirv.data(), tsv.spirv.size()), 4, 0U);
    REQUIRE(tpipe != nullptr);

    crd::containers::Array<float> tpar(&alloc);
    tpar.resize(8U, 0.0F);
    tpar[0] = static_cast<float>(imw); tpar[1] = static_cast<float>(imh); tpar[2] = static_cast<float>(tiles_x);
    tpar[3] = 0.03F; tpar[4] = 0.035F; tpar[5] = 0.05F; tpar[6] = 1.0F / 255.0F;
    crd::containers::Array<float> timgf(&alloc);
    timgf.resize(uz(imw * imh) * 4U, 0.0F);
    {
        float*    hb[4] = {buckets.data(), counts.data(), tpar.data(), timgf.data()};
        const int ln[4] = {n_tiles * cap * 12, n_tiles, 8, imw * imh * 4};
        crd::kir_test::dispatch_kernel_1wg(compute, *tpipe, hb, ln, 4, static_cast<crd::u32>((imw * imh + 63) / 64));
    }
    double worst = 0.0;
    for (int p = 0; p < imw * imh; ++p)
    {
        for (int c = 0; c < 3; ++c) { worst = crd::math::abs(static_cast<double>(timgf[uz(p) * 4U + uz(c)] - imgf[uz(p) * 4U + uz(c)])) > worst ? crd::math::abs(static_cast<double>(timgf[uz(p) * 4U + uz(c)] - imgf[uz(p) * 4U + uz(c)])) : worst; }
    }
    std::printf("[B19-a2] tiled render on GPU: %d tiles (%dx%d px), worst |tiled - brute| = %.2e\n", n_tiles, tile_px, tile_px, worst);
    CHECK(worst < 1.0e-3); // same image, GPU-emitted both ways (f32)
}

// D-007 B19-a3: the SORT HALF of 3DGS runs ON-DEVICE, bit-exact. project -> depthkey -> 4-pass KEY-VALUE radix sort
// (ckir_sort.hpp, carry_val) -> gather. Distinct linearly-spaced depths => distinct 24-bit keys => the GPU stable radix
// order is the UNIQUE depth order, so the gathered projected buffer must equal a host depth-sort of the SAME GPU-projected
// buffer, splat-for-splat. This removes the host depth-sort crutch the showcase used -- the whole splat pipeline is on-device.
TEST_CASE("B19-a3: on-device depth sort (project->depthkey->KV radix sort->gather) on Vulkan == host sort", "[gpu-context][vulkan][gpu][gsplat][sort]")
{
    namespace cg = crd::gpu;
    crd::memory::TlsfAllocator alloc(128U << 20U, nullptr, "gsplat-gpusort");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int n            = 4096;             // = nblocks(4) * epb(1024)
    constexpr int threads      = 256;
    constexpr int radix_bits   = 8;
    constexpr int nbins        = 256;
    constexpr int epb          = 1024;
    constexpr int nblocks      = n / epb;
    constexpr int scan_threads = nblocks < threads ? nblocks : threads;
    const double  c0          = kir::gsplat::detail::kShC0;

    // scene: n splats at DISTINCT linearly-spaced depths over [3,9]; x,y scattered (irrelevant to the sort)
    crd::containers::Array<float> gauss(&alloc);
    gauss.resize(uz(n) * 14U, 0.0F);
    crd::u32   st = 0x51A7EDU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n; ++i)
    {
        const double z = 3.0 + 6.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(n);
        float*       q = gauss.data() + uz(i) * 14U;
        q[0] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.5); q[1] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.5); q[2] = static_cast<float>(z);
        q[3] = q[4] = q[5] = 0.05F;
        q[6] = 0.0F; q[7] = 0.0F; q[8] = 0.0F; q[9] = 1.0F;
        q[10] = 0.7F;
        q[11] = static_cast<float>((0.6 - 0.5) / c0); q[12] = static_cast<float>((0.4 - 0.5) / c0); q[13] = 0.0F; // colour (0.6,0.4,0.5) → SH0; blue 0.5 ⇒ coeff 0
    }
    // camera: R=identity, t=0 => view-z == world z => depth == z directly (so [dmin,dmax]=[2.5,9.5] brackets the scene).
    crd::containers::Array<float> cam(&alloc);
    cam.resize(20U, 0.0F);
    cam[0] = 1.0F; cam[4] = 1.0F; cam[8] = 1.0F;
    cam[12] = 100.0F; cam[13] = 100.0F; cam[14] = 32.0F; cam[15] = 32.0F;
    cam[16] = 0.2F; cam[17] = 64.0F; cam[18] = 64.0F;
    crd::containers::Array<float> par(&alloc);
    par.resize(3U, 0.0F); par[0] = 2.5F; par[1] = 9.5F; par[2] = static_cast<float>(n);

    // compile every stage of the pipeline
    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph pg(&alloc);
    kir::KGraph dg(&alloc);
    kir::KGraph gof1(&alloc);
    kir::KGraph gof2(&alloc);
    kir::KGraph gg(&alloc);
    auto p_proj = mk(pg, kir::gsplat::build_gsplat_project_kernel(pg, {}), 3, "gs_project");
    auto p_key  = mk(dg, kir::gsplat::build_gsplat_depthkey_kernel(dg, {}), 4, "gs_depthkey");
    auto p_off1 = mk(gof1, kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads), 3, "gs_off1");
    auto p_off2 = mk(gof2, kir::build_sort_gbase(gof2, radix_bits), 2, "gs_gbase");
    auto p_gath = mk(gg, kir::gsplat::build_gsplat_gather_kernel(gg), 3, "gs_gather");
    std::unique_ptr<cg::ComputePipeline> ph_s[4];
    std::unique_ptr<cg::ComputePipeline> ps_s[4];
    kir::KGraph ghg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    kir::KGraph gsg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    for (int p = 0; p < 4; ++p)
    {
        ph_s[p] = mk(ghg[p], kir::build_sort_histogram(ghg[p], epb, threads, radix_bits, p * 8, nblocks), 2, "gs_hist");
        ps_s[p] = mk(gsg[p], kir::build_sort_scatter(gsg[p], epb, threads, radix_bits, p * 8, nblocks, true), 6, "gs_scat");
        REQUIRE(ph_s[p] != nullptr); REQUIRE(ps_s[p] != nullptr);
    }
    REQUIRE(p_proj != nullptr); REQUIRE(p_key != nullptr); REQUIRE(p_off1 != nullptr); REQUIRE(p_off2 != nullptr); REQUIRE(p_gath != nullptr);

    // device buffers (all 4-byte elements)
    const auto dbuf = [&](crd::u64 elems) { return compute.create_buffer(elems * 4U, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); };
    auto d_gauss  = dbuf(uz(n) * 14U);
    auto d_cam    = dbuf(20U);
    auto d_proj   = dbuf(uz(n) * 12U);
    auto d_par    = dbuf(3U);
    auto d_ka     = dbuf(uz(n)); auto d_kb = dbuf(uz(n));
    auto d_va     = dbuf(uz(n)); auto d_vb = dbuf(uz(n));
    auto d_hist   = dbuf(uz(nblocks * nbins));
    auto d_off    = dbuf(uz(nblocks * nbins));
    auto d_tot    = dbuf(uz(nbins));
    auto d_gb     = dbuf(uz(nbins));
    auto d_sorted = dbuf(uz(n) * 12U);

    // upload gauss / cam / par
    const auto upload = [&](cg::ComputeBuffer& dev, const float* src, int len) {
        auto stg = compute.create_buffer(static_cast<crd::u64>(len) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p  = static_cast<float*>(stg->map());
        for (int i = 0; i < len; ++i) { p[i] = src[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dev, 0U, 0U, static_cast<crd::u64>(len) * 4U);
        compute.submit_and_wait();
    };
    upload(*d_gauss, gauss.data(), n * 14);
    upload(*d_cam, cam.data(), 20);
    upload(*d_par, par.data(), 3);

    // the whole pipeline in one command buffer
    auto&      rec = compute.begin();
    const auto bar = [&](cg::ComputeBuffer& b) { rec.barrier(b, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); };
    const auto disp = [&](cg::ComputePipeline& pipe, cg::ComputeBuffer** bufs, int nb, crd::u32 gx) {
        rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(bufs, static_cast<crd::usize>(nb)), nullptr, 0U, gx, 1U, 1U);
    };
    const crd::u32 grid_lin = static_cast<crd::u32>((n + 63) / 64);
    // project
    cg::ComputeBuffer* pb[3] = {d_gauss.get(), d_cam.get(), d_proj.get()};
    disp(*p_proj, pb, 3, grid_lin);
    bar(*d_proj);
    // depthkey -> keys in d_ka, vals in d_va
    cg::ComputeBuffer* kb[4] = {d_proj.get(), d_par.get(), d_ka.get(), d_va.get()};
    disp(*p_key, kb, 4, grid_lin);
    bar(*d_ka); bar(*d_va);
    // 4 LSD passes, ping-ponging keys (ka/kb) AND vals (va/vb)
    cg::ComputeBuffer* ck = d_ka.get(); cg::ComputeBuffer* ok = d_kb.get();
    cg::ComputeBuffer* cv = d_va.get(); cg::ComputeBuffer* ov = d_vb.get();
    for (int p = 0; p < 4; ++p)
    {
        cg::ComputeBuffer* hb[2] = {ck, d_hist.get()};
        disp(*ph_s[p], hb, 2, static_cast<crd::u32>(nblocks));
        bar(*d_hist);
        cg::ComputeBuffer* o1[3] = {d_hist.get(), d_off.get(), d_tot.get()};
        disp(*p_off1, o1, 3, static_cast<crd::u32>(nbins));
        bar(*d_off); bar(*d_tot);
        cg::ComputeBuffer* o2[2] = {d_tot.get(), d_gb.get()};
        disp(*p_off2, o2, 2, 1U);
        bar(*d_gb);
        cg::ComputeBuffer* sb[6] = {ck, ok, d_off.get(), d_gb.get(), cv, ov};
        disp(*ps_s[p], sb, 6, static_cast<crd::u32>(nblocks));
        bar(*ok); bar(*ov);
        cg::ComputeBuffer* tk = ck; ck = ok; ok = tk;
        cg::ComputeBuffer* tv = cv; cv = ov; ov = tv;
    }
    // gather: sorted[i] = proj[order[i]], order = cv (final sorted index payload)
    cg::ComputeBuffer* gb2[3] = {d_proj.get(), cv, d_sorted.get()};
    disp(*p_gath, gb2, 3, grid_lin);
    rec.barrier(*d_sorted, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
    rec.barrier(*d_proj, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
    rec.barrier(*cv, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);

    auto rb_sorted = compute.create_buffer(static_cast<crd::u64>(n) * 12U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    auto rb_proj   = compute.create_buffer(static_cast<crd::u64>(n) * 12U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    auto rb_ord    = compute.create_buffer(static_cast<crd::u64>(n) * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    rec.copy(*d_sorted, *rb_sorted, 0U, 0U, static_cast<crd::u64>(n) * 12U * 4U);
    rec.copy(*d_proj, *rb_proj, 0U, 0U, static_cast<crd::u64>(n) * 12U * 4U);
    rec.copy(*cv, *rb_ord, 0U, 0U, static_cast<crd::u64>(n) * 4U);
    compute.submit_and_wait();

    const auto* gsort = static_cast<const float*>(rb_sorted->map());
    const auto* gproj = static_cast<const float*>(rb_proj->map());
    const auto* gord  = static_cast<const crd::u32*>(rb_ord->map());

    // host reference: stable insertion sort of the indices by depth (slot 2) over the SAME GPU-projected buffer.
    crd::containers::Array<crd::u32> ho(&alloc);
    ho.resize(uz(n), 0U);
    for (int i = 0; i < n; ++i) { ho[uz(i)] = static_cast<crd::u32>(i); }
    for (int i = 1; i < n; ++i)
    {
        const crd::u32 key = ho[uz(i)];
        const float    kd  = gproj[uz(static_cast<int>(key)) * 12U + 2U];
        int            j   = i - 1;
        while (j >= 0 && gproj[uz(static_cast<int>(ho[uz(j)])) * 12U + 2U] > kd) { ho[uz(j + 1)] = ho[uz(j)]; --j; }
        ho[uz(j + 1)] = key;
    }

    int mism = 0; int bad_asc = 0; crd::u32 xperm = 0U;
    for (int i = 0; i < n; ++i)
    {
        const int src = static_cast<int>(ho[uz(i)]);
        for (int k = 0; k < 12; ++k)
        {
            if (gsort[uz(i) * 12U + uz(k)] != gproj[uz(src) * 12U + uz(k)]) { ++mism; }
        }
        if (i > 0 && gsort[uz(i) * 12U + 2U] < gsort[uz(i - 1) * 12U + 2U]) { ++bad_asc; }
        xperm ^= gord[i] ^ static_cast<crd::u32>(i); // order is a permutation => XOR(order) == XOR(0..n-1)
    }
    rb_sorted->unmap(); rb_proj->unmap(); rb_ord->unmap();
    INFO("mismatched sorted slots = " << mism << " / " << (n * 12) << ", non-ascending = " << bad_asc);
    CHECK(mism == 0);     // the GPU sort == the host sort, splat-for-splat (distinct keys => unique order)
    CHECK(bad_asc == 0);  // depths are nearest-first
    CHECK(xperm == 0U);   // the sorted index payload is a permutation of 0..n-1
}

// D-007 B19-a4: the FULL GPU TILE BINNING runs ON-DEVICE on real Vulkan. Host-project + host-depth-sort feed the input
// (both are gated elsewhere — projection closed-form, the depth sort in B19-a3); the a4 pipeline then runs entirely on
// the GPU: tilecount -> exclusive scan -> scatter (tile,splat) instances -> radix-sort BY TILE -> per-tile ranges ->
// BLOCK RENDER (one workgroup per tile, variable range length). The block image must equal the GPU brute render
// splat-for-splat. This exercises the new GLSL emissions: the guarded scatter `If`, the nested-`If` ranges kernel, and
// the block render's VARIABLE (buffer-loaded) `For` bound — the topology B19-a2's fixed cap could not express.
TEST_CASE("B19-a4: full GPU tile binning + block render on Vulkan == brute render (bit-exact)", "[gpu-context][vulkan][gpu][gsplat][bin]")
{
    namespace cg = crd::gpu;
    crd::memory::TlsfAllocator alloc(192U << 20U, nullptr, "gsplat-bin-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int n           = 256;
    constexpr int imw         = 64;
    constexpr int imh         = 64;
    constexpr int tile_px     = 16;
    constexpr int tiles_x     = imw / tile_px;              // 4
    constexpr int n_tiles     = tiles_x * (imh / tile_px);  // 16
    constexpr int max_cover   = 16;
    constexpr int local       = 64;
    constexpr int n_pad       = 2048;
    constexpr int threads     = 256;
    constexpr int radix_bits  = 8;
    constexpr int nbins       = 256;
    constexpr int epb         = 1024;
    constexpr int nblocks     = n_pad / epb;
    constexpr int scan_threads = nblocks < threads ? nblocks : threads;
    const double  pos_c       = 0.5 / kir::gsplat::detail::kShC0;
    const double  neg_c       = -0.5 / kir::gsplat::detail::kShC0;

    // scene (as doubles for the host project/oracle) → project + depth-sort on the host to produce `sorted` (the a4 input).
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);
    gauss.resize(uz(n) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 100.0; cam[13] = 100.0; cam[14] = 32.0; cam[15] = 32.0;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);
    crd::u32   sr = 0xB1A4EU;
    const auto rnd = [&]() { sr = sr * 1664525U + 1013904223U; return static_cast<double>(sr >> 8U) / 16777216.0; };
    const auto pg2 = [&](int i, double mx, double my, double mz, double s, double op, double r, double gg2, double bb) {
        const crd::usize o = uz(i) * 14U;
        gauss[o + 0U] = mx; gauss[o + 1U] = my; gauss[o + 2U] = mz;
        gauss[o + 3U] = s; gauss[o + 4U] = s; gauss[o + 5U] = s;
        gauss[o + 6U] = 0.0; gauss[o + 7U] = 0.0; gauss[o + 8U] = 0.0; gauss[o + 9U] = 1.0;
        gauss[o + 10U] = op; gauss[o + 11U] = r; gauss[o + 12U] = gg2; gauss[o + 13U] = bb;
    };
    for (int i = 0; i < n; ++i)
    {
        const double x = (rnd() * 2.0 - 1.0) * 1.0;
        const double y = (rnd() * 2.0 - 1.0) * 1.0;
        const double z = -1.2 + rnd() * 2.4;
        const double s = 0.05 + rnd() * 0.18;
        pg2(i, x, y, z, s, 0.6 + rnd() * 0.3, rnd() > 0.5 ? pos_c : neg_c, neg_c, rnd() > 0.5 ? pos_c : neg_c);
    }
    {
        kir::gsplat::GsplatProjectConfig pcfg;
        kir::KGraph                      hg(&alloc);
        const kir::KEntry                he = kir::gsplat::build_gsplat_project_kernel(hg, pcfg);
        proj.resize(uz(n) * 12U, 0.0);
        kir::KernelBuffer hb[3] = {{gauss.data(), n * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), n * 12, 0, 2}};
        kir::eval_cpu_kernel(hg, he, hb, 3, he.local_size[0], &alloc, 1U);
    }
    crd::containers::Array<int> ord(&alloc);
    ord.resize(uz(n), 0);
    for (int i = 0; i < n; ++i) { ord[uz(i)] = i; }
    for (int i = 1; i < n; ++i)
    {
        const int    key = ord[uz(i)];
        const double kd  = proj[uz(key) * 12U + 2U];
        int          j   = i - 1;
        while (j >= 0 && proj[uz(ord[uz(j)]) * 12U + 2U] > kd) { ord[uz(j + 1)] = ord[uz(j)]; --j; }
        ord[uz(j + 1)] = key;
    }
    crd::containers::Array<float> sorted(&alloc);
    sorted.resize(uz(n) * 12U, 0.0F);
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < 12; ++k) { sorted[uz(i) * 12U + uz(k)] = static_cast<float>(proj[uz(ord[uz(i)]) * 12U + uz(k)]); }
    }

    kir::gsplat::GsplatBinConfig   bcfg;
    bcfg.width = imw; bcfg.height = imh; bcfg.tile_px = tile_px; bcfg.max_cover = max_cover; bcfg.local_size = local;
    kir::gsplat::GsplatBlockConfig blkcfg;
    blkcfg.width = imw; blkcfg.height = imh; blkcfg.tile_px = tile_px;
    kir::gsplat::GsplatRenderConfig brcfg;
    brcfg.width = imw; brcfg.height = imh; brcfg.max_splats = n;

    // compile
    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph gtc(&alloc);
    kir::KGraph gsc(&alloc);
    kir::KGraph grg(&alloc);
    kir::KGraph gbl(&alloc);
    kir::KGraph gbr(&alloc);
    kir::KGraph gs0(&alloc);
    kir::KGraph gs1(&alloc);
    kir::KGraph gs2(&alloc);
    kir::KGraph* sgp[3] = {&gs0, &gs1, &gs2};
    const kir::ScanPlan plan = kir::build_scan(sgp, n, false, 256, 1);
    REQUIRE(plan.single_pass);
    auto p_tc  = mk(gtc, kir::gsplat::build_gsplat_tilecount_kernel(gtc, bcfg), 2, "gs_tilecount");
    auto p_scan = mk(*plan.block_graph, plan.block, 2, "gs_scan");
    auto p_sca = mk(gsc, kir::gsplat::build_gsplat_scatter_instances_kernel(gsc, bcfg), 5, "gs_scatter");
    auto p_rng = mk(grg, kir::gsplat::build_gsplat_tile_ranges_kernel(grg, local), 3, "gs_ranges");
    auto p_blk = mk(gbl, kir::gsplat::build_gsplat_block_render_kernel(gbl, blkcfg), 5, "gs_block");
    auto p_bru = mk(gbr, kir::gsplat::build_gsplat_render_kernel(gbr, brcfg), 3, "gs_brute");
    std::unique_ptr<cg::ComputePipeline> ph_s[4];
    std::unique_ptr<cg::ComputePipeline> ps_s[4];
    std::unique_ptr<cg::ComputePipeline> po1_s;
    std::unique_ptr<cg::ComputePipeline> po2_s;
    kir::KGraph gof1(&alloc);
    kir::KGraph gof2(&alloc);
    po1_s = mk(gof1, kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads), 3, "gs_off1");
    po2_s = mk(gof2, kir::build_sort_gbase(gof2, radix_bits), 2, "gs_gbase");
    kir::KGraph ghg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    kir::KGraph gsg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    for (int p = 0; p < 4; ++p)
    {
        ph_s[p] = mk(ghg[p], kir::build_sort_histogram(ghg[p], epb, threads, radix_bits, p * 8, nblocks), 2, "gs_hist");
        ps_s[p] = mk(gsg[p], kir::build_sort_scatter(gsg[p], epb, threads, radix_bits, p * 8, nblocks, true), 6, "gs_scat");
    }

    // device buffers
    const auto dbuf = [&](crd::u64 elems) { return compute.create_buffer(elems * 4U, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); };
    auto d_sorted = dbuf(uz(n) * 12U);
    auto d_tc     = dbuf(uz(n));
    auto d_off    = dbuf(uz(n));
    auto d_ka = dbuf(n_pad); auto d_kb = dbuf(n_pad);
    auto d_va = dbuf(n_pad); auto d_vb = dbuf(n_pad);
    auto d_hist = dbuf(uz(nblocks * nbins));
    auto d_o    = dbuf(uz(nblocks * nbins));
    auto d_tot  = dbuf(uz(nbins));
    auto d_gb   = dbuf(uz(nbins));
    auto d_rng  = dbuf(uz(n_tiles) * 2U);
    auto d_rpar = dbuf(1U);
    auto d_bpar = dbuf(4U);
    auto d_img  = dbuf(uz(imw * imh) * 4U);
    auto d_brpar = dbuf(8U);
    auto d_brimg = dbuf(uz(imw * imh) * 4U);

    const auto upload_f = [&](cg::ComputeBuffer& dev, const float* src, int len) {
        auto stg = compute.create_buffer(static_cast<crd::u64>(len) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p  = static_cast<float*>(stg->map());
        for (int i = 0; i < len; ++i) { p[i] = src[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dev, 0U, 0U, static_cast<crd::u64>(len) * 4U);
        compute.submit_and_wait();
    };
    const auto upload_u = [&](cg::ComputeBuffer& dev, crd::u32 fill, int len) {
        auto stg = compute.create_buffer(static_cast<crd::u64>(len) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p  = static_cast<crd::u32*>(stg->map());
        for (int i = 0; i < len; ++i) { p[i] = fill; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dev, 0U, 0U, static_cast<crd::u64>(len) * 4U);
        compute.submit_and_wait();
    };
    upload_f(*d_sorted, sorted.data(), n * 12);
    crd::containers::Array<float> bparh(&alloc);
    bparh.resize(4U, 0.0F); bparh[0] = 0.02F; bparh[1] = 0.03F; bparh[2] = 0.04F; bparh[3] = 1.0F / 255.0F;
    upload_f(*d_bpar, bparh.data(), 4);
    crd::containers::Array<float> brparh(&alloc);
    brparh.resize(8U, 0.0F); brparh[0] = static_cast<float>(n); brparh[1] = static_cast<float>(imw); brparh[2] = static_cast<float>(imh);
    brparh[3] = 0.02F; brparh[4] = 0.03F; brparh[5] = 0.04F; brparh[6] = 1.0F / 255.0F;
    upload_f(*d_brpar, brparh.data(), 8);

    const auto bar_w = [&](cg::ComputeRecorder& rec, cg::ComputeBuffer& b) { rec.barrier(b, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); };
    const auto disp = [&](cg::ComputeRecorder& rec, cg::ComputePipeline& pipe, cg::ComputeBuffer** bufs, int nb, crd::u32 gx) {
        rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(bufs, static_cast<crd::usize>(nb)), nullptr, 0U, gx, 1U, 1U);
    };

    // SUBMIT A: tilecount + scan → read back tc,off → T
    {
        auto& rec = compute.begin();
        cg::ComputeBuffer* tb[2] = {d_sorted.get(), d_tc.get()};
        disp(rec, *p_tc, tb, 2, static_cast<crd::u32>(n / local));
        bar_w(rec, *d_tc);
        cg::ComputeBuffer* sb[2] = {d_tc.get(), d_off.get()};
        disp(rec, *p_scan, sb, 2, static_cast<crd::u32>(plan.nblocks));
        rec.barrier(*d_tc, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
        rec.barrier(*d_off, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
        compute.submit_and_wait();
    }
    int total = 0;
    {
        auto rt = compute.create_buffer(static_cast<crd::u64>(n) * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
        auto ro = compute.create_buffer(static_cast<crd::u64>(n) * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
        auto& rec = compute.begin();
        rec.copy(*d_tc, *rt, 0U, 0U, static_cast<crd::u64>(n) * 4U);
        rec.copy(*d_off, *ro, 0U, 0U, static_cast<crd::u64>(n) * 4U);
        compute.submit_and_wait();
        const auto* tcp = static_cast<const float*>(rt->map());
        const auto* ofp = static_cast<const float*>(ro->map());
        total = static_cast<int>(ofp[n - 1] + tcp[n - 1]);
        rt->unmap(); ro->unmap();
    }
    REQUIRE(total > 0);
    REQUIRE(total < n_pad);

    // pre-fill the sort key buffer with the sentinel, pre-zero ranges, upload T
    upload_u(*d_ka, 0xFFFFFFFFU, n_pad);
    upload_u(*d_va, 0U, n_pad);
    upload_u(*d_rng, 0U, n_tiles * 2);
    crd::containers::Array<float> rparh(&alloc);
    rparh.resize(1U, 0.0F); rparh[0] = static_cast<float>(total);
    upload_f(*d_rpar, rparh.data(), 1);

    // SUBMIT B: scatter → 4-pass KV sort by tile → ranges → block render + brute render
    {
        auto& rec = compute.begin();
        cg::ComputeBuffer* scb[5] = {d_sorted.get(), d_tc.get(), d_off.get(), d_ka.get(), d_va.get()};
        disp(rec, *p_sca, scb, 5, static_cast<crd::u32>(n * max_cover / local));
        bar_w(rec, *d_ka); bar_w(rec, *d_va);
        cg::ComputeBuffer* ck = d_ka.get(); cg::ComputeBuffer* ok = d_kb.get();
        cg::ComputeBuffer* cv = d_va.get(); cg::ComputeBuffer* ov = d_vb.get();
        for (int p = 0; p < 4; ++p)
        {
            cg::ComputeBuffer* hb[2] = {ck, d_hist.get()};
            disp(rec, *ph_s[p], hb, 2, static_cast<crd::u32>(nblocks));
            bar_w(rec, *d_hist);
            cg::ComputeBuffer* o1[3] = {d_hist.get(), d_o.get(), d_tot.get()};
            disp(rec, *po1_s, o1, 3, static_cast<crd::u32>(nbins));
            bar_w(rec, *d_o); bar_w(rec, *d_tot);
            cg::ComputeBuffer* o2[2] = {d_tot.get(), d_gb.get()};
            disp(rec, *po2_s, o2, 2, 1U);
            bar_w(rec, *d_gb);
            cg::ComputeBuffer* sb[6] = {ck, ok, d_o.get(), d_gb.get(), cv, ov};
            disp(rec, *ps_s[p], sb, 6, static_cast<crd::u32>(nblocks));
            bar_w(rec, *ok); bar_w(rec, *ov);
            cg::ComputeBuffer* tk = ck; ck = ok; ok = tk;
            cg::ComputeBuffer* tv = cv; cv = ov; ov = tv;
        }
        // ck = sorted tile keys, cv = sorted splat-index payload (the block render's order)
        cg::ComputeBuffer* rgb[3] = {ck, d_rpar.get(), d_rng.get()};
        disp(rec, *p_rng, rgb, 3, static_cast<crd::u32>(n_pad / local));
        bar_w(rec, *d_rng);
        cg::ComputeBuffer* blb[5] = {d_sorted.get(), cv, d_rng.get(), d_bpar.get(), d_img.get()};
        disp(rec, *p_blk, blb, 5, static_cast<crd::u32>(n_tiles));
        cg::ComputeBuffer* brb[3] = {d_sorted.get(), d_brpar.get(), d_brimg.get()};
        disp(rec, *p_bru, brb, 3, static_cast<crd::u32>(imw * imh / local));
        rec.barrier(*d_img, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
        rec.barrier(*d_brimg, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
        compute.submit_and_wait();
    }

    auto ri = compute.create_buffer(static_cast<crd::u64>(imw * imh) * 4U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    auto rbr = compute.create_buffer(static_cast<crd::u64>(imw * imh) * 4U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    {
        auto& rec = compute.begin();
        rec.copy(*d_img, *ri, 0U, 0U, static_cast<crd::u64>(imw * imh) * 4U * 4U);
        rec.copy(*d_brimg, *rbr, 0U, 0U, static_cast<crd::u64>(imw * imh) * 4U * 4U);
        compute.submit_and_wait();
    }
    const auto* bi = static_cast<const float*>(ri->map());
    const auto* br = static_cast<const float*>(rbr->map());
    float worst = 0.0F;
    float lum   = 0.0F;
    for (int q = 0; q < imw * imh; ++q)
    {
        for (int c = 0; c < 3; ++c)
        {
            const float d = bi[uz(q) * 4U + uz(c)] > br[uz(q) * 4U + uz(c)] ? bi[uz(q) * 4U + uz(c)] - br[uz(q) * 4U + uz(c)] : br[uz(q) * 4U + uz(c)] - bi[uz(q) * 4U + uz(c)];
            if (d > worst) { worst = d; }
            lum += bi[uz(q) * 4U + uz(c)];
        }
    }
    ri->unmap(); rbr->unmap();
    std::printf("[B19-a4 GPU] full on-device bin: %d splats, T=%d over %d tiles; mean lum %.4f; worst |block - brute| = %.3e\n",
                n, total, n_tiles, lum / static_cast<float>(imw * imh * 3), worst);
    CHECK(lum > 0.0F);        // the block render actually drew something
    CHECK(worst == 0.0F);     // GPU block render == GPU brute render, splat-for-splat (bit-exact f32)
}
