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
#include <crd/kir/ckir_gsplat2d.hpp>
#include <crd/kir/ckir_mesh.hpp>
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

// D-007 B19-c: the 2D GAUSSIAN SPLATTING surfel primitive runs ON THE GPU. Both kernels (project + ray-surfel render)
// dispatch on real Vulkan and match the CPU oracle: the project prepares the view-space surfel, the render solves the
// ray-surfel intersection per pixel and writes colour + the depth/normal geometry G-buffer. Host depth-sort between them
// (the a3/a4 machinery already sorts on-device). Same graph on both paths ⇒ the GPU floats match the oracle.
TEST_CASE("B19-c: 2DGS surfel project + ray-surfel render on Vulkan == CPU oracle", "[gpu-context][vulkan][gpu][gsplat2d]")
{
    crd::memory::TlsfAllocator alloc(128U << 20U, nullptr, "gsplat2d-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr int imw = 64;
    constexpr int imh = 64;
    constexpr int ns  = 24;
    const double  c0  = kir::gsplat::detail::kShC0;

    // scene: surfels scattered in view, each with a random orientation (so depth/normal genuinely vary per pixel).
    crd::containers::Array<float> surf(&alloc);
    surf.resize(uz(ns) * 13U, 0.0F);
    crd::u32   st = 0x2D65U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < ns; ++i)
    {
        float* q = surf.data() + uz(i) * 13U;
        q[0] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.8);
        q[1] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.8);
        q[2] = static_cast<float>(-1.0 + rnd() * 2.0);
        q[3] = static_cast<float>(0.25 + rnd() * 0.35); // su
        q[4] = static_cast<float>(0.25 + rnd() * 0.35); // sv
        // random unit quaternion
        const double a1 = rnd() * 6.2831853; const double a2 = rnd() * 6.2831853; const double u1 = rnd();
        q[5] = static_cast<float>(crd::math::sqrt(1.0 - u1) * crd::math::sin(a1));
        q[6] = static_cast<float>(crd::math::sqrt(1.0 - u1) * crd::math::cos(a1));
        q[7] = static_cast<float>(crd::math::sqrt(u1) * crd::math::sin(a2));
        q[8] = static_cast<float>(crd::math::sqrt(u1) * crd::math::cos(a2));
        q[9] = 0.85F;
        q[10] = static_cast<float>((rnd() - 0.5) / c0); q[11] = static_cast<float>((rnd() - 0.5) / c0); q[12] = static_cast<float>((rnd() - 0.5) / c0);
    }
    crd::containers::Array<float> cam(&alloc);
    cam.resize(20U, 0.0F);
    cam[0] = 1.0F; cam[4] = 1.0F; cam[8] = 1.0F;
    cam[9] = 0.0F; cam[10] = 0.0F; cam[11] = 5.0F;
    cam[12] = 90.0F; cam[13] = 90.0F; cam[14] = 32.0F; cam[15] = 32.0F;
    cam[16] = 0.2F; cam[17] = static_cast<float>(imw); cam[18] = static_cast<float>(imh);

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<crd::gpu::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };

    // GPU project
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    kir::KGraph                        pg(&alloc);
    auto p_proj = mk(pg, kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg), 3, "gs2d_project");
    REQUIRE(p_proj != nullptr);
    crd::containers::Array<float> prep(&alloc);
    prep.resize(uz(ns) * 19U, 0.0F);
    {
        float*    hb[3] = {surf.data(), cam.data(), prep.data()};
        const int ln[3] = {ns * 13, 20, ns * 19};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_proj, hb, ln, 3, static_cast<crd::u32>((ns + 63) / 64));
    }

    // CPU oracle project (as doubles) for the prep comparison
    crd::containers::Array<double> surfd(&alloc);
    crd::containers::Array<double> camd(&alloc);
    surfd.resize(uz(ns) * 13U, 0.0); camd.resize(20U, 0.0);
    for (int i = 0; i < ns * 13; ++i) { surfd[uz(i)] = surf[uz(i)]; }
    for (int i = 0; i < 20; ++i) { camd[uz(i)] = cam[uz(i)]; }
    kir::KGraph       pg2(&alloc);
    const kir::KEntry pe2 = kir::gsplat::build_gsplat2d_project_kernel(pg2, pcfg);
    crd::containers::Array<double> prep_ref(&alloc);
    prep_ref.resize(uz(ns) * 19U, 0.0);
    kir::KernelBuffer pbb[3] = {{surfd.data(), ns * 13, 0, 0}, {camd.data(), 20, 0, 1}, {prep_ref.data(), ns * 19, 0, 2}};
    kir::eval_cpu_kernel(pg2, pe2, pbb, 3, pe2.local_size[0], &alloc, 1U);

    float worst_prep = 0.0F;
    for (int i = 0; i < ns * 19; ++i)
    {
        const float d = crd::math::abs(prep[uz(i)] - static_cast<float>(prep_ref[uz(i)]));
        if (d > worst_prep) { worst_prep = d; }
    }
    CHECK(worst_prep < 1.0e-3F); // GPU project == oracle project

    // host depth sort the GPU-projected surfels (slot 12), nearest-first
    crd::containers::Array<int> ord(&alloc);
    ord.resize(uz(ns), 0);
    for (int i = 0; i < ns; ++i) { ord[uz(i)] = i; }
    for (int i = 1; i < ns; ++i)
    {
        const int   key = ord[uz(i)];
        const float kd  = prep[uz(key) * 19U + 12U];
        int         j   = i - 1;
        while (j >= 0 && prep[uz(ord[uz(j)]) * 19U + 12U] > kd)
        {
            const int jp1 = j + 1;
            ord[uz(jp1)] = ord[uz(j)];
            --j;
        }
        const int jp1 = j + 1;
        ord[uz(jp1)] = key;
    }
    crd::containers::Array<float>  sorted(&alloc);
    crd::containers::Array<double> sortedd(&alloc);
    sorted.resize(uz(ns) * 19U, 0.0F);
    sortedd.resize(uz(ns) * 19U, 0.0);
    for (int i = 0; i < ns; ++i)
    {
        for (int k = 0; k < 19; ++k)
        {
            sorted[uz(i) * 19U + uz(k)]  = prep[uz(ord[uz(i)]) * 19U + uz(k)];
            sortedd[uz(i) * 19U + uz(k)] = sorted[uz(i) * 19U + uz(k)];
        }
    }

    // GPU render
    kir::gsplat::Gsplat2dRenderConfig rcfg;
    rcfg.width = imw; rcfg.height = imh; rcfg.max_splats = ns;
    kir::KGraph rg(&alloc);
    auto p_rend = mk(rg, kir::gsplat::build_gsplat2d_render_kernel(rg, rcfg), 4, "gs2d_render");
    REQUIRE(p_rend != nullptr);
    crd::containers::Array<float> par(&alloc);
    par.resize(5U, 0.0F); par[0] = static_cast<float>(ns); par[1] = 0.02F; par[2] = 0.02F; par[3] = 0.03F; par[4] = 1.0F / 255.0F;
    crd::containers::Array<float> img(&alloc);
    img.resize(uz(imw * imh) * 8U, 0.0F);
    {
        float*    hb[4] = {sorted.data(), cam.data(), par.data(), img.data()};
        const int ln[4] = {ns * 19, 20, 5, imw * imh * 8};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_rend, hb, ln, 4, static_cast<crd::u32>((imw * imh + 63) / 64));
    }

    // CPU oracle render (same sorted surfels)
    kir::KGraph       rg2(&alloc);
    const kir::KEntry re2 = kir::gsplat::build_gsplat2d_render_kernel(rg2, rcfg);
    crd::containers::Array<double> pard(&alloc);
    pard.resize(5U, 0.0); for (int i = 0; i < 5; ++i) { pard[uz(i)] = par[uz(i)]; }
    crd::containers::Array<double> img_ref(&alloc);
    img_ref.resize(uz(imw * imh) * 8U, 0.0);
    kir::KernelBuffer rbb[4] = {{sortedd.data(), ns * 19, 0, 0}, {camd.data(), 20, 0, 1}, {pard.data(), 5, 0, 2}, {img_ref.data(), imw * imh * 8, 0, 3}};
    kir::eval_cpu_kernel(rg2, re2, rbb, 4, re2.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    float worst = 0.0F;
    float lum   = 0.0F;
    for (int q = 0; q < imw * imh * 8; ++q)
    {
        const float d = crd::math::abs(img[uz(q)] - static_cast<float>(img_ref[uz(q)]));
        if (d > worst) { worst = d; }
    }
    for (int p = 0; p < imw * imh; ++p) { lum += img[uz(p) * 8U + 0U] + img[uz(p) * 8U + 1U] + img[uz(p) * 8U + 2U]; }
    std::printf("[B19-c GPU] 2DGS %d surfels %dx%d: mean lum %.4f; worst |GPU render - oracle| = %.3e (prep %.3e)\n",
                ns, imw, imh, lum / static_cast<float>(imw * imh * 3), worst, worst_prep);
    CHECK(lum > 0.0F);
    CHECK(worst < 2.0e-3F); // GPU ray-surfel render (colour + depth + normal) == oracle
}

// D-007 B19-c2: TSDF FUSION runs ON THE GPU. A posed plane depth map integrated into a Truncated Signed Distance Field
// on a voxel grid, dispatched on real Vulkan, matches the CPU oracle bit-for-bit — the fused field is the signed ramp
// whose zero crossing is the surface. (Marching cubes turns this field into a mesh next.)
TEST_CASE("B19-c2: TSDF fusion on Vulkan == CPU oracle (signed ramp on a voxel grid)", "[gpu-context][vulkan][gpu][mesh]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U, nullptr, "tsdf-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr int nx = 8;
    constexpr int ny = 8;
    constexpr int nz = 16;
    constexpr int nvox = nx * ny * nz;
    constexpr int imw = 32;
    constexpr int imh = 32;

    crd::containers::Array<float> depth(&alloc);
    depth.resize(uz(imw * imh), 0.0F);
    for (int i = 0; i < imw * imh; ++i) { depth[uz(i)] = 5.0F; } // a plane at view-z 5
    crd::containers::Array<float> cam(&alloc);
    cam.resize(20U, 0.0F);
    cam[0] = 1.0F; cam[4] = 1.0F; cam[8] = 1.0F;
    cam[12] = 30.0F; cam[13] = 30.0F; cam[14] = 16.0F; cam[15] = 16.0F;
    cam[16] = 0.2F; cam[17] = static_cast<float>(imw); cam[18] = static_cast<float>(imh);
    crd::containers::Array<float> gp(&alloc);
    gp.resize(5U, 0.0F); gp[0] = -1.0F; gp[1] = -1.0F; gp[2] = 3.0F; gp[3] = 0.25F; gp[4] = 1.0F;

    kir::mesh::TsdfConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz; cfg.img_w = imw; cfg.img_h = imh;
    kir::KGraph       tg(&alloc);
    const kir::KEntry te = kir::mesh::build_tsdf_integrate_kernel(tg, cfg);
    kir::GlslKernel   tk(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(tg, te, &alloc, tk));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(tk.source), "tsdf", &alloc);
    INFO("tsdf GLSL: " << spv.error_message.c_str());
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> tsum(&alloc);
    crd::containers::Array<float> wsum(&alloc);
    tsum.resize(uz(nvox), 0.0F);
    wsum.resize(uz(nvox), 0.0F);
    {
        float*    hb[5] = {depth.data(), cam.data(), gp.data(), tsum.data(), wsum.data()};
        const int ln[5] = {imw * imh, 20, 5, nvox, nvox};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hb, ln, 5, static_cast<crd::u32>(nvox / 64));
    }

    // CPU oracle (doubles)
    crd::containers::Array<double> depthd(&alloc);
    crd::containers::Array<double> camd(&alloc);
    crd::containers::Array<double> gpd(&alloc);
    crd::containers::Array<double> tsr(&alloc);
    crd::containers::Array<double> wsr(&alloc);
    depthd.resize(uz(imw * imh), 0.0); for (int i = 0; i < imw * imh; ++i) { depthd[uz(i)] = 5.0; }
    camd.resize(20U, 0.0); for (int i = 0; i < 20; ++i) { camd[uz(i)] = cam[uz(i)]; }
    gpd.resize(5U, 0.0); for (int i = 0; i < 5; ++i) { gpd[uz(i)] = gp[uz(i)]; }
    tsr.resize(uz(nvox), 0.0); wsr.resize(uz(nvox), 0.0);
    kir::KGraph       tg2(&alloc);
    const kir::KEntry te2 = kir::mesh::build_tsdf_integrate_kernel(tg2, cfg);
    kir::KernelBuffer bb[5] = {{depthd.data(), imw * imh, 0, 0}, {camd.data(), 20, 0, 1}, {gpd.data(), 5, 0, 2}, {tsr.data(), nvox, 0, 3}, {wsr.data(), nvox, 0, 4}};
    kir::eval_cpu_kernel(tg2, te2, bb, 5, te2.local_size[0], &alloc, static_cast<crd::u32>(nvox / 64));

    float worst = 0.0F;
    int   observed = 0;
    for (int i = 0; i < nvox; ++i)
    {
        const float dt = crd::math::abs(tsum[uz(i)] - static_cast<float>(tsr[uz(i)]));
        const float dw = crd::math::abs(wsum[uz(i)] - static_cast<float>(wsr[uz(i)]));
        if (dt > worst) { worst = dt; }
        if (dw > worst) { worst = dw; }
        if (wsum[uz(i)] > 0.5F) { ++observed; }
    }
    std::printf("[B19-c2 GPU] TSDF %dx%dx%d: %d observed voxels; worst |GPU - oracle| = %.3e\n", nx, ny, nz, observed, worst);
    CHECK(observed > 0);       // the grid saw the surface
    CHECK(worst < 1.0e-4F);    // GPU TSDF == oracle
}

// D-007 B19-c2b: MARCHING CUBES runs ON THE GPU. The full extract pipeline — count → scan → emit — dispatched on real
// Vulkan over an analytic sphere SDF, producing a triangle mesh (positions + outward normals) that matches the CPU
// oracle mesh vertex-for-vertex. This closes the mesh bridge: a fused field becomes real geometry on-device.
TEST_CASE("B19-c2b: marching cubes on Vulkan == CPU oracle (sphere mesh)", "[gpu-context][vulkan][gpu][mesh]")
{
    namespace cg = crd::gpu;
    crd::memory::TlsfAllocator alloc(160U << 20U, nullptr, "mc-gpu");
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

    constexpr int nx = 9;
    constexpr int ny = 9;
    constexpr int nz = 9;
    constexpr int nvox = nx * ny * nz;
    constexpr int ncells = (nx - 1) * (ny - 1) * (nz - 1); // 512
    constexpr int max_tris = ncells * 5;
    const double  h  = 0.25;
    const double  o  = -1.0;
    const double  rr = 0.5;

    // analytic sphere SDF field
    crd::containers::Array<float> field(&alloc);
    field.resize(uz(nvox), 0.0F);
    for (int k = 0; k < nz; ++k)
    {
        for (int j = 0; j < ny; ++j)
        {
            for (int i = 0; i < nx; ++i)
            {
                const double x = o + (static_cast<double>(i) + 0.5) * h;
                const double y = o + (static_cast<double>(j) + 0.5) * h;
                const double z = o + (static_cast<double>(k) + 0.5) * h;
                field[uz(k * nx * ny + j * nx + i)] = static_cast<float>(crd::math::sqrt(x * x + y * y + z * z) - rr);
            }
        }
    }
    crd::containers::Array<float> gpm(&alloc);
    gpm.resize(4U, 0.0F); gpm[0] = static_cast<float>(o); gpm[1] = static_cast<float>(o); gpm[2] = static_cast<float>(o); gpm[3] = static_cast<float>(h);

    kir::mesh::McConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;

    // ── CPU ORACLE mesh (count -> scan -> emit) ──
    crd::containers::Array<double> fieldd(&alloc);
    fieldd.resize(uz(nvox), 0.0); for (int i = 0; i < nvox; ++i) { fieldd[uz(i)] = field[uz(i)]; }
    crd::containers::Array<double> trid(&alloc); trid.resize(256U * 16U, 0.0);
    for (int i = 0; i < 256 * 16; ++i) { trid[uz(i)] = static_cast<double>(kir::mesh::kMcTriTable[i]); }
    crd::containers::Array<double> econd(&alloc); crd::containers::Array<double> cofd(&alloc);
    econd.resize(24U, 0.0); cofd.resize(24U, 0.0);
    for (int i = 0; i < 24; ++i) { econd[uz(i)] = static_cast<double>(kir::mesh::kMcEdgeConn[i]); cofd[uz(i)] = static_cast<double>(kir::mesh::kMcCornerOff[i]); }
    crd::containers::Array<double> gpmd(&alloc); gpmd.resize(4U, 0.0); for (int i = 0; i < 4; ++i) { gpmd[uz(i)] = gpm[uz(i)]; }
    crd::containers::Array<double> countd(&alloc); countd.resize(uz(ncells), 0.0);
    kir::KGraph cgo(&alloc);
    const kir::KEntry ceo = kir::mesh::build_mc_count_kernel(cgo, cfg);
    kir::KernelBuffer cbo[3] = {{fieldd.data(), nvox, 0, 0}, {trid.data(), 256 * 16, 0, 1}, {countd.data(), ncells, 0, 2}};
    kir::eval_cpu_kernel(cgo, ceo, cbo, 3, ceo.local_size[0], &alloc, static_cast<crd::u32>(ncells / 64));
    kir::KGraph s0(&alloc); kir::KGraph s1(&alloc); kir::KGraph s2(&alloc);
    kir::KGraph* sgo[3] = {&s0, &s1, &s2};
    const kir::ScanPlan plano = kir::build_scan(sgo, ncells, false, 256, 1);
    crd::containers::Array<double> offd(&alloc); offd.resize(uz(ncells), 0.0);
    kir::KernelBuffer sbo[2] = {{countd.data(), ncells, 0, 0}, {offd.data(), ncells, 0, 1}};
    kir::eval_cpu_kernel(*plano.block_graph, plano.block, sbo, 2, plano.block.local_size[0], &alloc, static_cast<crd::u32>(plano.nblocks));
    const int total = static_cast<int>(offd[uz(ncells - 1)] + countd[uz(ncells - 1)]);
    REQUIRE(total > 50);
    REQUIRE(total <= max_tris);
    crd::containers::Array<double> outd(&alloc); outd.resize(uz(total) * 18U, 0.0);
    kir::KGraph ego(&alloc);
    const kir::KEntry eeo = kir::mesh::build_mc_emit_kernel(ego, cfg);
    kir::KernelBuffer ebo[7] = {{fieldd.data(), nvox, 0, 0}, {gpmd.data(), 4, 0, 1}, {trid.data(), 256 * 16, 0, 2},
                                {econd.data(), 24, 0, 3}, {cofd.data(), 24, 0, 4}, {offd.data(), ncells, 0, 5}, {outd.data(), total * 18, 0, 6}};
    kir::eval_cpu_kernel(ego, eeo, ebo, 7, eeo.local_size[0], &alloc, static_cast<crd::u32>(ncells / 64));

    // ── GPU pipeline ──
    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph cg2(&alloc); kir::KGraph eg2(&alloc); kir::KGraph gs0(&alloc); kir::KGraph gs1(&alloc); kir::KGraph gs2(&alloc);
    kir::KGraph* sgp[3] = {&gs0, &gs1, &gs2};
    const kir::ScanPlan plang = kir::build_scan(sgp, ncells, false, 256, 1);
    auto p_count = mk(cg2, kir::mesh::build_mc_count_kernel(cg2, cfg), 3, "mc_count");
    auto p_scan  = mk(*plang.block_graph, plang.block, 2, "mc_scan");
    auto p_emit  = mk(eg2, kir::mesh::build_mc_emit_kernel(eg2, cfg), 7, "mc_emit");
    REQUIRE(p_count != nullptr); REQUIRE(p_scan != nullptr); REQUIRE(p_emit != nullptr);

    const auto dbuf = [&](crd::u64 bytes) { return compute.create_buffer(bytes, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); };
    auto d_field = dbuf(uz(nvox) * 4U);
    auto d_tri   = dbuf(256U * 16U * 4U);
    auto d_gp    = dbuf(4U * 4U);
    auto d_econ  = dbuf(24U * 4U);
    auto d_coff  = dbuf(24U * 4U);
    auto d_count = dbuf(uz(ncells) * 4U);
    auto d_off   = dbuf(uz(ncells) * 4U);
    auto d_out   = dbuf(uz(max_tris) * 18U * 4U);

    const auto up_f = [&](cg::ComputeBuffer& dev, const float* src, int len) {
        auto stg = compute.create_buffer(static_cast<crd::u64>(len) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p = static_cast<float*>(stg->map()); for (int i = 0; i < len; ++i) { p[i] = src[i]; } stg->unmap();
        auto& rc = compute.begin(); rc.copy(*stg, dev, 0U, 0U, static_cast<crd::u64>(len) * 4U); compute.submit_and_wait();
    };
    const auto up_i = [&](cg::ComputeBuffer& dev, const int* src, int len) {
        auto stg = compute.create_buffer(static_cast<crd::u64>(len) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p = static_cast<crd::i32*>(stg->map()); for (int i = 0; i < len; ++i) { p[i] = src[i]; } stg->unmap();
        auto& rc = compute.begin(); rc.copy(*stg, dev, 0U, 0U, static_cast<crd::u64>(len) * 4U); compute.submit_and_wait();
    };
    up_f(*d_field, field.data(), nvox);
    up_f(*d_gp, gpm.data(), 4);
    up_i(*d_tri, kir::mesh::kMcTriTable, 256 * 16);
    up_i(*d_econ, kir::mesh::kMcEdgeConn, 24);
    up_i(*d_coff, kir::mesh::kMcCornerOff, 24);

    {
        auto& rec = compute.begin();
        const auto bar = [&](cg::ComputeBuffer& b) { rec.barrier(b, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); };
        cg::ComputeBuffer* cb[3] = {d_field.get(), d_tri.get(), d_count.get()};
        rec.dispatch(*p_count, crd::containers::ConstSpan<cg::ComputeBuffer*>(cb, 3), nullptr, 0U, static_cast<crd::u32>(ncells / 64), 1U, 1U);
        bar(*d_count);
        cg::ComputeBuffer* sb[2] = {d_count.get(), d_off.get()};
        rec.dispatch(*p_scan, crd::containers::ConstSpan<cg::ComputeBuffer*>(sb, 2), nullptr, 0U, static_cast<crd::u32>(plang.nblocks), 1U, 1U);
        bar(*d_off);
        cg::ComputeBuffer* eb[7] = {d_field.get(), d_gp.get(), d_tri.get(), d_econ.get(), d_coff.get(), d_off.get(), d_out.get()};
        rec.dispatch(*p_emit, crd::containers::ConstSpan<cg::ComputeBuffer*>(eb, 7), nullptr, 0U, static_cast<crd::u32>(ncells / 64), 1U, 1U);
        rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
        compute.submit_and_wait();
    }

    auto rb = compute.create_buffer(static_cast<crd::u64>(total) * 18U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    {
        auto& rec = compute.begin();
        rec.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(total) * 18U * 4U);
        compute.submit_and_wait();
    }
    const auto* g = static_cast<const float*>(rb->map());
    float worst = 0.0F;
    for (int i = 0; i < total * 18; ++i)
    {
        const float d = crd::math::abs(g[uz(i)] - static_cast<float>(outd[uz(i)]));
        if (d > worst) { worst = d; }
    }
    rb->unmap();
    std::printf("[B19-c2b GPU] marching cubes: %d triangles; worst |GPU mesh - oracle| = %.3e\n", total, worst);
    CHECK(worst < 1.0e-4F); // GPU marching cubes == oracle, vertex for vertex
}

// D-007 B19-e: RELIGHTABLE 2DGS on the GPU. The PBR shade (Lambert + GGX under a directional light, using the surfel's
// intrinsic normal) dispatches on real Vulkan and matches the CPU oracle — captured content re-lit on-device.
TEST_CASE("B19-e: relightable 2DGS render on Vulkan == CPU oracle", "[gpu-context][vulkan][gpu][gsplat2d]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U, nullptr, "gsplat2d-relight-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr int imw = 64;
    constexpr int imh = 64;
    constexpr int ns  = 16;

    crd::containers::Array<float> surf(&alloc);
    surf.resize(uz(ns) * 13U, 0.0F);
    crd::u32   st = 0x2E11U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < ns; ++i)
    {
        float* q = surf.data() + uz(i) * 13U;
        q[0] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.6); q[1] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.6); q[2] = static_cast<float>(-0.5 + rnd());
        q[3] = static_cast<float>(0.3 + rnd() * 0.3); q[4] = static_cast<float>(0.3 + rnd() * 0.3);
        const double a1 = rnd() * 6.2831853; const double a2 = rnd() * 6.2831853; const double u1 = rnd();
        q[5] = static_cast<float>(crd::math::sqrt(1.0 - u1) * crd::math::sin(a1)); q[6] = static_cast<float>(crd::math::sqrt(1.0 - u1) * crd::math::cos(a1));
        q[7] = static_cast<float>(crd::math::sqrt(u1) * crd::math::sin(a2)); q[8] = static_cast<float>(crd::math::sqrt(u1) * crd::math::cos(a2));
        q[9] = 0.9F; q[10] = static_cast<float>(rnd()); q[11] = static_cast<float>(rnd()); q[12] = static_cast<float>(rnd());
    }
    crd::containers::Array<float> cam(&alloc);
    cam.resize(20U, 0.0F);
    cam[0] = 1.0F; cam[4] = 1.0F; cam[8] = 1.0F; cam[11] = 5.0F;
    cam[12] = 90.0F; cam[13] = 90.0F; cam[14] = 32.0F; cam[15] = 32.0F;
    cam[16] = 0.2F; cam[17] = static_cast<float>(imw); cam[18] = static_cast<float>(imh);
    crd::containers::Array<float> par(&alloc);
    par.resize(13U, 0.0F);
    par[0] = static_cast<float>(ns); par[1] = 0.02F; par[2] = 0.02F; par[3] = 0.03F;
    par[4] = 0.3F; par[5] = 0.4F; par[6] = -1.0F; par[7] = 1.0F; par[8] = 0.95F; par[9] = 0.9F; par[10] = 0.05F; par[11] = 0.35F; par[12] = 1.0F / 255.0F;

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<crd::gpu::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    kir::KGraph pg(&alloc);
    auto p_proj = mk(pg, kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg), 3, "gs2d_proj");
    crd::containers::Array<float> prep(&alloc);
    prep.resize(uz(ns) * 19U, 0.0F);
    {
        float* hb[3] = {surf.data(), cam.data(), prep.data()};
        const int ln[3] = {ns * 13, 20, ns * 19};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_proj, hb, ln, 3, static_cast<crd::u32>((ns + 63) / 64));
    }
    // host depth sort
    crd::containers::Array<int> ord(&alloc); ord.resize(uz(ns), 0);
    for (int i = 0; i < ns; ++i) { ord[uz(i)] = i; }
    for (int i = 1; i < ns; ++i)
    {
        const int key = ord[uz(i)]; const float kd = prep[uz(key) * 19U + 12U]; int j = i - 1;
        while (j >= 0 && prep[uz(ord[uz(j)]) * 19U + 12U] > kd) { const int jp1 = j + 1; ord[uz(jp1)] = ord[uz(j)]; --j; }
        const int jp1 = j + 1; ord[uz(jp1)] = key;
    }
    crd::containers::Array<float>  sorted(&alloc); crd::containers::Array<double> sortedd(&alloc);
    sorted.resize(uz(ns) * 19U, 0.0F); sortedd.resize(uz(ns) * 19U, 0.0);
    for (int i = 0; i < ns; ++i) { for (int k = 0; k < 19; ++k) { sorted[uz(i) * 19U + uz(k)] = prep[uz(ord[uz(i)]) * 19U + uz(k)]; sortedd[uz(i) * 19U + uz(k)] = sorted[uz(i) * 19U + uz(k)]; } }

    kir::gsplat::Gsplat2dRelightConfig rcfg;
    rcfg.width = imw; rcfg.height = imh; rcfg.max_splats = ns;
    kir::KGraph rg(&alloc);
    auto p_rl = mk(rg, kir::gsplat::build_gsplat2d_relight_render_kernel(rg, rcfg), 4, "gs2d_relight");
    crd::containers::Array<float> img(&alloc); img.resize(uz(imw * imh) * 4U, 0.0F);
    {
        float* hb[4] = {sorted.data(), cam.data(), par.data(), img.data()};
        const int ln[4] = {ns * 19, 20, 13, imw * imh * 4};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_rl, hb, ln, 4, static_cast<crd::u32>((imw * imh + 63) / 64));
    }
    // CPU oracle
    crd::containers::Array<double> camd(&alloc); crd::containers::Array<double> pard(&alloc);
    camd.resize(20U, 0.0); for (int i = 0; i < 20; ++i) { camd[uz(i)] = cam[uz(i)]; }
    pard.resize(13U, 0.0); for (int i = 0; i < 13; ++i) { pard[uz(i)] = par[uz(i)]; }
    kir::KGraph rg2(&alloc);
    const kir::KEntry re2 = kir::gsplat::build_gsplat2d_relight_render_kernel(rg2, rcfg);
    crd::containers::Array<double> imgref(&alloc); imgref.resize(uz(imw * imh) * 4U, 0.0);
    kir::KernelBuffer rbb[4] = {{sortedd.data(), ns * 19, 0, 0}, {camd.data(), 20, 0, 1}, {pard.data(), 13, 0, 2}, {imgref.data(), imw * imh * 4, 0, 3}};
    kir::eval_cpu_kernel(rg2, re2, rbb, 4, re2.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    float worst = 0.0F; float lum = 0.0F;
    for (int q = 0; q < imw * imh * 4; ++q) { const float d = crd::math::abs(img[uz(q)] - static_cast<float>(imgref[uz(q)])); if (d > worst) { worst = d; } }
    for (int p = 0; p < imw * imh; ++p) { lum += img[uz(p) * 4U + 0U] + img[uz(p) * 4U + 1U] + img[uz(p) * 4U + 2U]; }
    std::printf("[B19-e GPU] relightable 2DGS %d surfels %dx%d: mean lum %.4f; worst |GPU - oracle| = %.3e\n", ns, imw, imh, lum / static_cast<float>(imw * imh * 3), worst);
    CHECK(lum > 0.0F);
    CHECK(worst < 2.0e-3F); // GPU PBR relight == oracle
}

// D-007 B19 StopThePop: the PER-PIXEL RESORT render (nested loops + a per-pixel scratch buffer for the O(N²) selection)
// dispatches on real Vulkan and matches the CPU oracle. Proves the nested-For + scratch-RMW construct lowers to GLSL.
TEST_CASE("B19 StopThePop: per-pixel resort render on Vulkan == CPU oracle", "[gpu-context][vulkan][gpu][gsplat2d]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U, nullptr, "gsplat2d-resort-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr int imw = 48;
    constexpr int imh = 48;
    constexpr int ns  = 8;
    const double  c0  = kir::gsplat::detail::kShC0;

    crd::containers::Array<float> surf(&alloc);
    surf.resize(uz(ns) * 13U, 0.0F);
    crd::u32   st = 0x570BU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < ns; ++i)
    {
        float* q = surf.data() + uz(i) * 13U;
        q[0] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.5); q[1] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.5); q[2] = static_cast<float>(-0.4 + rnd() * 0.8);
        q[3] = static_cast<float>(0.5 + rnd() * 0.5); q[4] = static_cast<float>(0.5 + rnd() * 0.5);
        const double a1 = rnd() * 6.2831853; const double a2 = rnd() * 6.2831853; const double u1 = rnd();
        q[5] = static_cast<float>(crd::math::sqrt(1.0 - u1) * crd::math::sin(a1)); q[6] = static_cast<float>(crd::math::sqrt(1.0 - u1) * crd::math::cos(a1));
        q[7] = static_cast<float>(crd::math::sqrt(u1) * crd::math::sin(a2)); q[8] = static_cast<float>(crd::math::sqrt(u1) * crd::math::cos(a2));
        q[9] = 0.7F; q[10] = static_cast<float>((rnd() - 0.5) / c0); q[11] = static_cast<float>((rnd() - 0.5) / c0); q[12] = static_cast<float>((rnd() - 0.5) / c0);
    }
    crd::containers::Array<float> cam(&alloc);
    cam.resize(20U, 0.0F);
    cam[0] = 1.0F; cam[4] = 1.0F; cam[8] = 1.0F; cam[11] = 5.0F;
    cam[12] = 70.0F; cam[13] = 70.0F; cam[14] = 24.0F; cam[15] = 24.0F;
    cam[16] = 0.2F; cam[17] = static_cast<float>(imw); cam[18] = static_cast<float>(imh);

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<crd::gpu::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    kir::KGraph pg(&alloc);
    auto p_proj = mk(pg, kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg), 3, "gs2d_proj");
    crd::containers::Array<float> prep(&alloc);
    prep.resize(uz(ns) * 19U, 0.0F);
    {
        float* hb[3] = {surf.data(), cam.data(), prep.data()};
        const int ln[3] = {ns * 13, 20, ns * 19};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_proj, hb, ln, 3, static_cast<crd::u32>((ns + 63) / 64));
    }
    crd::containers::Array<double> prepd(&alloc); prepd.resize(uz(ns) * 19U, 0.0);
    for (int i = 0; i < ns * 19; ++i) { prepd[uz(i)] = prep[uz(i)]; }

    kir::gsplat::Gsplat2dResortConfig rrc;
    rrc.width = imw; rrc.height = imh; rrc.max_splats = ns;
    kir::KGraph rg(&alloc);
    auto p_rs = mk(rg, kir::gsplat::build_gsplat2d_resort_render_kernel(rg, rrc), 5, "gs2d_resort");
    crd::containers::Array<float> par(&alloc);
    par.resize(5U, 0.0F); par[0] = static_cast<float>(ns); par[4] = 1.0F / 255.0F;
    crd::containers::Array<float> img(&alloc); crd::containers::Array<float> scr(&alloc);
    img.resize(uz(imw * imh) * 4U, 0.0F); scr.resize(uz(imw * imh) * 4U, 0.0F);
    {
        float* hb[5] = {prep.data(), cam.data(), par.data(), img.data(), scr.data()};
        const int ln[5] = {ns * 19, 20, 5, imw * imh * 4, imw * imh * 4};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_rs, hb, ln, 5, static_cast<crd::u32>((imw * imh + 63) / 64));
    }
    // CPU oracle
    crd::containers::Array<double> camd(&alloc); crd::containers::Array<double> pard(&alloc);
    camd.resize(20U, 0.0); for (int i = 0; i < 20; ++i) { camd[uz(i)] = cam[uz(i)]; }
    pard.resize(5U, 0.0); pard[0] = static_cast<double>(ns); pard[4] = 1.0 / 255.0;
    kir::KGraph rg2(&alloc);
    const kir::KEntry re2 = kir::gsplat::build_gsplat2d_resort_render_kernel(rg2, rrc);
    crd::containers::Array<double> imgref(&alloc); crd::containers::Array<double> scrref(&alloc);
    imgref.resize(uz(imw * imh) * 4U, 0.0); scrref.resize(uz(imw * imh) * 4U, 0.0);
    kir::KernelBuffer rbb[5] = {{prepd.data(), ns * 19, 0, 0}, {camd.data(), 20, 0, 1}, {pard.data(), 5, 0, 2}, {imgref.data(), imw * imh * 4, 0, 3}, {scrref.data(), imw * imh * 4, 0, 4}};
    kir::eval_cpu_kernel(rg2, re2, rbb, 5, re2.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    float worst = 0.0F; float lum = 0.0F;
    for (int q = 0; q < imw * imh * 4; ++q) { const float d = crd::math::abs(img[uz(q)] - static_cast<float>(imgref[uz(q)])); if (d > worst) { worst = d; } }
    for (int p = 0; p < imw * imh; ++p) { lum += img[uz(p) * 4U + 0U] + img[uz(p) * 4U + 1U] + img[uz(p) * 4U + 2U]; }
    std::printf("[B19 StopThePop GPU] resort %d surfels %dx%d: mean lum %.4f; worst |GPU - oracle| = %.3e\n", ns, imw, imh, lum / static_cast<float>(imw * imh * 3), worst);
    CHECK(lum > 0.0F);
    CHECK(worst < 2.0e-3F); // GPU per-pixel resort == oracle
}

// D-007 B19-d: the Gaussian COMPRESSION codec on the GPU. Quantise → dequantise (the K-bit attribute codec) dispatches
// on real Vulkan and reconstructs == the CPU oracle. (Morton reorder is bit-ops, oracle-gated.)
TEST_CASE("B19-d: quantise/dequantise codec on Vulkan == CPU oracle", "[gpu-context][vulkan][gpu][gsplat]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "gsplat-quant-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr int n    = 256;
    constexpr int natt = 14;
    constexpr int bits = 12;

    crd::containers::Array<float> gs(&alloc);
    gs.resize(uz(n) * natt, 0.0F);
    crd::u32 st = 0xC0FFEEU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n * natt; ++i) { gs[uz(i)] = static_cast<float>((rnd() * 2.0 - 1.0) * 2.0); }
    crd::containers::Array<float> rng(&alloc);
    rng.resize(uz(natt) * 2U, 0.0F);
    for (int k = 0; k < natt; ++k)
    {
        float lo = 1.0e30F; float hi = -1.0e30F;
        for (int i = 0; i < n; ++i) { const float v = gs[uz(i * natt + k)]; if (v < lo) { lo = v; } if (v > hi) { hi = v; } }
        rng[uz(k) * 2U] = lo; rng[uz(k) * 2U + 1U] = hi;
    }

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<crd::gpu::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::gsplat::GsplatQuantizeConfig qc;
    qc.natt = natt; qc.bits = bits;
    kir::KGraph qg(&alloc); kir::KGraph dg(&alloc);
    auto p_q = mk(qg, kir::gsplat::build_gsplat_quantize_kernel(qg, qc), 3, "gs_quant");
    auto p_d = mk(dg, kir::gsplat::build_gsplat_dequantize_kernel(dg, qc), 3, "gs_dequant");

    crd::containers::Array<float> codes(&alloc); crd::containers::Array<float> recon(&alloc);
    codes.resize(uz(n) * natt, 0.0F); recon.resize(uz(n) * natt, 0.0F);
    {
        float* hb[3] = {gs.data(), rng.data(), codes.data()};
        const int ln[3] = {n * natt, natt * 2, n * natt};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_q, hb, ln, 3, static_cast<crd::u32>((n + 63) / 64));
    }
    {
        float* hb[3] = {codes.data(), rng.data(), recon.data()};
        const int ln[3] = {n * natt, natt * 2, n * natt};
        crd::kir_test::dispatch_kernel_1wg(compute, *p_d, hb, ln, 3, static_cast<crd::u32>((n + 63) / 64));
    }

    // CPU oracle
    crd::containers::Array<double> gsd(&alloc); crd::containers::Array<double> rngd(&alloc);
    gsd.resize(uz(n) * natt, 0.0); rngd.resize(uz(natt) * 2U, 0.0);
    for (int i = 0; i < n * natt; ++i) { gsd[uz(i)] = gs[uz(i)]; }
    for (int i = 0; i < natt * 2; ++i) { rngd[uz(i)] = rng[uz(i)]; }
    crd::containers::Array<double> cref(&alloc); crd::containers::Array<double> rref(&alloc);
    cref.resize(uz(n) * natt, 0.0); rref.resize(uz(n) * natt, 0.0);
    kir::KGraph qg2(&alloc); const kir::KEntry qe2 = kir::gsplat::build_gsplat_quantize_kernel(qg2, qc);
    kir::KernelBuffer qb[3] = {{gsd.data(), n * natt, 0, 0}, {rngd.data(), natt * 2, 0, 1}, {cref.data(), n * natt, 0, 2}};
    kir::eval_cpu_kernel(qg2, qe2, qb, 3, qe2.local_size[0], &alloc, static_cast<crd::u32>(n / 64));
    kir::KGraph dg2(&alloc); const kir::KEntry de2 = kir::gsplat::build_gsplat_dequantize_kernel(dg2, qc);
    kir::KernelBuffer db[3] = {{cref.data(), n * natt, 0, 0}, {rngd.data(), natt * 2, 0, 1}, {rref.data(), n * natt, 0, 2}};
    kir::eval_cpu_kernel(dg2, de2, db, 3, de2.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    float worst = 0.0F;
    for (int i = 0; i < n * natt; ++i) { const float d = crd::math::abs(recon[uz(i)] - static_cast<float>(rref[uz(i)])); if (d > worst) { worst = d; } }
    std::printf("[B19-d GPU] %d-bit codec on Vulkan: worst |GPU - oracle| = %.3e\n", bits, worst);
    CHECK(worst < 1.0e-4F); // GPU codec == oracle
}

// D-007 B19-f: DIFFERENTIABLE TRAINING on the GPU. The forward differentiable splat and the exact backward gradient
// reduction dispatch on real Vulkan and match the CPU oracle — the render+gradient loop that fits Gaussians runs on-device.
TEST_CASE("B19-f: differentiable forward + backward on Vulkan == CPU oracle", "[gpu-context][vulkan][gpu][gsplat]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "gsplat-diff-gpu");
    gpu::GpuContextConfig      gcfg;
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    kir::gsplat::GsplatDiffConfig cfg;
    cfg.width = 32; cfg.height = 32; cfg.local_size = 64;
    const int np = cfg.width * cfg.height;

    crd::containers::Array<float> tgtp(&alloc);
    tgtp.resize(5U, 0.0F); tgtp[0] = 16.0F; tgtp[1] = 16.0F; tgtp[2] = 5.0F; tgtp[3] = 0.9F; tgtp[4] = 0.8F;
    crd::containers::Array<float> params(&alloc);
    params.resize(5U, 0.0F); params[0] = 14.0F; params[1] = 18.0F; params[2] = 4.0F; params[3] = 0.6F; params[4] = 0.55F;

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<crd::gpu::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph fg(&alloc); kir::KGraph bg(&alloc);
    auto p_f = mk(fg, kir::gsplat::build_gsplat_diff_forward_kernel(fg, cfg), 2, "gs_dfwd");
    auto p_b = mk(bg, kir::gsplat::build_gsplat_diff_backward_kernel(bg, cfg), 3, "gs_dbwd");

    // target (GPU forward with target params)
    crd::containers::Array<float> target(&alloc); target.resize(uz(np), 0.0F);
    { float* hb[2] = {tgtp.data(), target.data()}; const int ln[2] = {5, np}; crd::kir_test::dispatch_kernel_1wg(compute, *p_f, hb, ln, 2, static_cast<crd::u32>((np + 63) / 64)); }
    // GPU forward at the test params + GPU backward gradient
    crd::containers::Array<float> img(&alloc); img.resize(uz(np), 0.0F);
    { float* hb[2] = {params.data(), img.data()}; const int ln[2] = {5, np}; crd::kir_test::dispatch_kernel_1wg(compute, *p_f, hb, ln, 2, static_cast<crd::u32>((np + 63) / 64)); }
    crd::containers::Array<float> grad(&alloc); grad.resize(5U, 0.0F);
    { float* hb[3] = {params.data(), target.data(), grad.data()}; const int ln[3] = {5, np, 5}; crd::kir_test::dispatch_kernel_1wg(compute, *p_b, hb, ln, 3, 1U); }

    // CPU oracle
    crd::containers::Array<double> pd(&alloc); crd::containers::Array<double> td(&alloc);
    pd.resize(5U, 0.0); for (int i = 0; i < 5; ++i) { pd[uz(i)] = params[uz(i)]; }
    td.resize(uz(np), 0.0); for (int i = 0; i < np; ++i) { td[uz(i)] = target[uz(i)]; }
    crd::containers::Array<double> imgref(&alloc); crd::containers::Array<double> gref(&alloc);
    imgref.resize(uz(np), 0.0); gref.resize(5U, 0.0);
    kir::KGraph fg2(&alloc); const kir::KEntry fe2 = kir::gsplat::build_gsplat_diff_forward_kernel(fg2, cfg);
    kir::KernelBuffer fb[2] = {{pd.data(), 5, 0, 0}, {imgref.data(), np, 0, 1}};
    kir::eval_cpu_kernel(fg2, fe2, fb, 2, fe2.local_size[0], &alloc, static_cast<crd::u32>(np / cfg.local_size));
    kir::KGraph bg2(&alloc); const kir::KEntry be2 = kir::gsplat::build_gsplat_diff_backward_kernel(bg2, cfg);
    kir::KernelBuffer bb[3] = {{pd.data(), 5, 0, 0}, {td.data(), np, 0, 1}, {gref.data(), 5, 0, 2}};
    kir::eval_cpu_kernel(bg2, be2, bb, 3, be2.local_size[0], &alloc, 1U);

    float wf = 0.0F; float wg = 0.0F;
    for (int i = 0; i < np; ++i) { const float d = crd::math::abs(img[uz(i)] - static_cast<float>(imgref[uz(i)])); if (d > wf) { wf = d; } }
    for (int k = 0; k < 5; ++k) { const float d = crd::math::abs(grad[uz(k)] - static_cast<float>(gref[uz(k)])); const float rel = d / (crd::math::abs(static_cast<float>(gref[uz(k)])) + 1.0e-4F); if (rel > wg) { wg = rel; } }
    std::printf("[B19-f GPU] diff forward+backward on Vulkan: worst |fwd - oracle| = %.3e, worst grad rel = %.3e\n", wf, wg);
    CHECK(wf < 1.0e-4F);   // GPU forward == oracle
    CHECK(wg < 1.0e-3F);   // GPU gradient == oracle
}

// D-007 B19 PERFORMANCE: the shared-memory tiled rasteriser at 1080p with MILLIONS of splats, GPU-timed, vs the direct
// block render. Proves Cerid does real-scale 3DGS AND measures the shared-memory optimisation. Scene binned host-side
// (counting sort by tile — untimed setup); the TIMED work is the render, min-of-N GPU-timestamped. Hidden ([.]).
TEST_CASE("B19 perf: shared-mem 3DGS rasteriser at 1080p, millions of splats -- GPU benchmark", "[.gsplat-bench]")
{
    namespace cg = crd::gpu;
    crd::memory::TlsfAllocator alloc(1536U << 20U, nullptr, "gsplat-bench");
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

    constexpr int imw     = 1920;
    constexpr int tile_px = 16;
    constexpr int tiles_x = (imw + tile_px - 1) / tile_px;       // 120
    constexpr int tiles_y = (1080 + tile_px - 1) / tile_px;      // 68
    constexpr int n_tiles = tiles_x * tiles_y;                   // 8160
    constexpr int padded_w = tiles_x * tile_px;                  // 1920
    constexpr int padded_h = tiles_y * tile_px;                  // 1088

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::gsplat::GsplatBlockConfig bc;
    bc.width = imw; bc.height = 1080; bc.tile_px = tile_px;
    kir::KGraph pg(&alloc);
    kir::KGraph dg(&alloc);
    kir::KGraph sg(&alloc);
    kir::gsplat::GsplatBlockConfig bc_et = bc;
    bc_et.early_terminate = true;
    kir::KGraph eg(&alloc);
    auto p_proj   = mk(pg, kir::gsplat::build_gsplat_project_kernel(pg, {}), 3, "proj");
    auto p_direct = mk(dg, kir::gsplat::build_gsplat_block_render_kernel(dg, bc), 5, "block_direct");
    auto p_smem   = mk(sg, kir::gsplat::build_gsplat_block_render_smem_kernel(sg, bc), 5, "block_smem");
    auto p_smem_et = mk(eg, kir::gsplat::build_gsplat_block_render_smem_kernel(eg, bc_et), 5, "block_smem_et");
    REQUIRE(p_proj != nullptr); REQUIRE(p_direct != nullptr); REQUIRE(p_smem != nullptr); REQUIRE(p_smem_et != nullptr);

    const auto dbuf = [&](crd::u64 bytes) { return compute.create_buffer(bytes, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); };

    const int scenes[2] = {1000000, 4000000};
    std::printf("\n=== B19 3DGS shared-mem rasteriser @ %dx1080 (%d tiles) ===\n", imw, n_tiles);
    for (int sc = 0; sc < 2; ++sc)
    {
        const int n = scenes[sc];
        // ── scene: n splats spread across the frame, moderate scale (realistic per-tile overdraw) ──
        crd::containers::Array<float> gauss(&alloc);
        gauss.resize(uz(n) * 14U, 0.0F);
        crd::u32 st = 0xBEE5U + static_cast<crd::u32>(sc);
        const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
        for (int i = 0; i < n; ++i)
        {
            float* q = gauss.data() + uz(i) * 14U;
            q[0] = static_cast<float>((rnd() * 2.0 - 1.0) * 1.6); q[1] = static_cast<float>((rnd() * 2.0 - 1.0) * 0.9); q[2] = static_cast<float>(1.0 + rnd() * 6.0);
            q[3] = q[4] = q[5] = static_cast<float>(0.01 + rnd() * 0.02);
            q[6] = 0.0F; q[7] = 0.0F; q[8] = 0.0F; q[9] = 1.0F;
            q[10] = static_cast<float>(0.2 + rnd() * 0.6);
            q[11] = static_cast<float>(rnd()); q[12] = static_cast<float>(rnd()); q[13] = static_cast<float>(rnd());
        }
        crd::containers::Array<float> cam(&alloc);
        cam.resize(20U, 0.0F);
        cam[0] = 1.0F; cam[4] = 1.0F; cam[8] = 1.0F; cam[11] = 0.5F;
        cam[12] = 1400.0F; cam[13] = 1400.0F; cam[14] = static_cast<float>(imw) * 0.5F; cam[15] = 540.0F;
        cam[16] = 0.2F; cam[17] = static_cast<float>(imw); cam[18] = 1080.0F;

        auto d_gauss = dbuf(uz(n) * 14U * 4U);
        auto d_cam   = dbuf(20U * 4U);
        auto d_proj  = dbuf(uz(n) * 12U * 4U);
        {
            auto stg = compute.create_buffer(uz(n) * 14U * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* p = static_cast<float*>(stg->map()); for (int i = 0; i < n * 14; ++i) { p[i] = gauss[uz(i)]; } stg->unmap();
            auto stc = compute.create_buffer(20U * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* pc = static_cast<float*>(stc->map()); for (int i = 0; i < 20; ++i) { pc[i] = cam[uz(i)]; } stc->unmap();
            auto& rc = compute.begin();
            rc.copy(*stg, *d_gauss, 0U, 0U, uz(n) * 14U * 4U); rc.copy(*stc, *d_cam, 0U, 0U, 20U * 4U);
            compute.submit_and_wait();
        }
        // project on GPU, read back
        {
            auto& rec = compute.begin();
            cg::ComputeBuffer* pb[3] = {d_gauss.get(), d_cam.get(), d_proj.get()};
            rec.dispatch(*p_proj, crd::containers::ConstSpan<cg::ComputeBuffer*>(pb, 3), nullptr, 0U, static_cast<crd::u32>((n + 63) / 64), 1U, 1U);
            rec.barrier(*d_proj, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            compute.submit_and_wait();
        }
        crd::containers::Array<float> proj(&alloc); proj.resize(uz(n) * 12U, 0.0F);
        {
            auto rb = compute.create_buffer(uz(n) * 12U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
            auto& rec = compute.begin(); rec.copy(*d_proj, *rb, 0U, 0U, uz(n) * 12U * 4U); compute.submit_and_wait();
            const auto* r = static_cast<const float*>(rb->map()); for (int i = 0; i < n * 12; ++i) { proj[uz(i)] = r[i]; } rb->unmap();
        }
        // ── host bin: counting sort by centre tile → order[] + ranges[] (UNTIMED setup) ──
        crd::containers::Array<crd::u32> cnt(&alloc); cnt.resize(uz(n_tiles), 0U);
        crd::containers::Array<int> tof(&alloc); tof.resize(uz(n), -1);
        for (int i = 0; i < n; ++i)
        {
            if (proj[uz(i) * 12U + 11U] <= 0.5F) { continue; }
            const int tx = static_cast<int>(proj[uz(i) * 12U + 0U]) / tile_px;
            const int ty = static_cast<int>(proj[uz(i) * 12U + 1U]) / tile_px;
            if (tx < 0 || tx >= tiles_x || ty < 0 || ty >= tiles_y) { continue; }
            const int t = ty * tiles_x + tx; tof[uz(i)] = t; ++cnt[uz(t)];
        }
        crd::containers::Array<crd::u32> ranges(&alloc); ranges.resize(uz(n_tiles) * 2U, 0U);
        crd::u32 acc = 0U;
        for (int t = 0; t < n_tiles; ++t) { ranges[uz(t) * 2U] = acc; acc += cnt[uz(t)]; ranges[uz(t) * 2U + 1U] = acc; }
        const int total = static_cast<int>(acc);
        crd::containers::Array<crd::u32> wptr(&alloc); wptr.resize(uz(n_tiles), 0U);
        for (int t = 0; t < n_tiles; ++t) { wptr[uz(t)] = ranges[uz(t) * 2U]; }
        crd::containers::Array<crd::u32> order(&alloc); order.resize(uz(total > 0 ? total : 1), 0U);
        for (int i = 0; i < n; ++i) { const int t = tof[uz(i)]; if (t >= 0) { order[static_cast<crd::usize>(wptr[uz(t)])] = static_cast<crd::u32>(i); ++wptr[uz(t)]; } }

        // upload sorted(=proj), order, ranges, params
        auto d_ord = dbuf(uz(total > 0 ? total : 1) * 4U);
        auto d_rng = dbuf(uz(n_tiles) * 2U * 4U);
        auto d_par = dbuf(4U * 4U);
        auto d_od  = dbuf(uz(padded_w * padded_h) * 4U * 4U); // direct out
        auto d_os  = dbuf(uz(padded_w * padded_h) * 4U * 4U); // smem out
        {
            const auto upu = [&](cg::ComputeBuffer& dev, const crd::u32* src, int len) {
                auto stg = compute.create_buffer(static_cast<crd::u64>(len) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
                auto* p = static_cast<crd::u32*>(stg->map()); for (int i = 0; i < len; ++i) { p[i] = src[i]; } stg->unmap();
                auto& rc = compute.begin(); rc.copy(*stg, dev, 0U, 0U, static_cast<crd::u64>(len) * 4U); compute.submit_and_wait();
            };
            upu(*d_ord, order.data(), total > 0 ? total : 1);
            upu(*d_rng, ranges.data(), n_tiles * 2);
            crd::containers::Array<float> par(&alloc); par.resize(4U, 0.0F); par[0] = 0.02F; par[1] = 0.02F; par[2] = 0.03F; par[3] = 1.0F / 255.0F;
            auto stp = compute.create_buffer(4U * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* pp = static_cast<float*>(stp->map()); for (int i = 0; i < 4; ++i) { pp[i] = par[uz(i)]; } stp->unmap();
            auto& rc = compute.begin(); rc.copy(*stp, *d_par, 0U, 0U, 4U * 4U); compute.submit_and_wait();
        }

        const auto time_render = [&](cg::ComputePipeline& pipe, cg::ComputeBuffer& outb) {
            const auto rec1 = [&]() {
                auto& rec = compute.begin();
                cg::ComputeBuffer* b[5] = {d_proj.get(), d_ord.get(), d_rng.get(), d_par.get(), &outb};
                rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(b, 5), nullptr, 0U, static_cast<crd::u32>(n_tiles), 1U, 1U);
                compute.submit_and_wait();
            };
            rec1(); rec1(); // warm
            double best = 1.0e30;
            for (int r = 0; r < 6; ++r) { rec1(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }
            return best;
        };
        auto d_oe  = dbuf(uz(padded_w * padded_h) * 4U * 4U); // smem + early-out out
        const double ms_d = time_render(*p_direct, *d_od);
        const double ms_s = time_render(*p_smem, *d_os);
        const double ms_e = time_render(*p_smem_et, *d_oe);

        // bit-exact check (smem == direct) + early-out approximation error (smem_et vs direct)
        double worst = 0.0; double worst_et = 0.0;
        {
            auto rbd = compute.create_buffer(uz(padded_w * padded_h) * 4U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
            auto rbs = compute.create_buffer(uz(padded_w * padded_h) * 4U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
            auto rbe = compute.create_buffer(uz(padded_w * padded_h) * 4U * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
            auto& rec = compute.begin();
            rec.copy(*d_od, *rbd, 0U, 0U, uz(padded_w * padded_h) * 4U * 4U);
            rec.copy(*d_os, *rbs, 0U, 0U, uz(padded_w * padded_h) * 4U * 4U);
            rec.copy(*d_oe, *rbe, 0U, 0U, uz(padded_w * padded_h) * 4U * 4U);
            compute.submit_and_wait();
            const auto* a = static_cast<const float*>(rbd->map());
            const auto* b = static_cast<const float*>(rbs->map());
            const auto* c = static_cast<const float*>(rbe->map());
            for (int i = 0; i < padded_w * padded_h * 4; i += 977)
            {
                const double d = crd::math::abs(static_cast<double>(a[i] - b[i])); if (d > worst) { worst = d; }
                const double e = crd::math::abs(static_cast<double>(a[i] - c[i])); if (e > worst_et) { worst_et = e; }
            }
            rbd->unmap(); rbs->unmap(); rbe->unmap();
        }
        std::printf("  N=%7d  T=%8d  |  direct %6.3f ms (%5.1f fps)  smem %6.3f ms (%6.1f fps) %.2fx  smem+earlyout %6.3f ms (%6.1f fps) %.2fx  |  %.0f Minst/s  |  exact %.0e  approx %.1e\n",
                    n, total, ms_d, 1000.0 / ms_d, ms_s, 1000.0 / ms_s, ms_d / ms_s, ms_e, 1000.0 / ms_e, ms_d / ms_e,
                    static_cast<double>(total) / (ms_e * 1000.0), worst, worst_et);
        CHECK(worst < 1.0e-5);      // smem (no early-out) == direct, bit-exact
        CHECK(worst_et < 5.0e-3);   // early-out drops only the saturated tail (≤ ~t_stop·colour)
    }
}
