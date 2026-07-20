// test_dx12_compute.cpp — the D3D12 IComputeContext (ADR-0100) on the GPU. Proves the ONE backend-agnostic dispatch
// surface is genuinely backend-neutral: the SAME crd::gpu::IComputeContext API that drives geometry + CKIR on Vulkan
// runs a real kernel on DirectX 12. Vector-add through the full portable path (upload → dispatch → copy-readback),
// verified vs the CPU. This is the second backend that the seam claims to support — here it is, running.

#include <crd/gpu/dx12_compute_context.hpp>

#include <crd/kir/ckir.hpp>        // B-cmp: KGraph/KEntry for the shared-memory compute kernel
#include <crd/kir/ckir_fft.hpp>    // B-cmp Phase 1: build_fft1d_radix2 (the CKIR FFT authoring layer)
#include <crd/kir/ckir_reduce.hpp> // B-cmp: build_reduce (the CKIR device-wide reduction)
#include <crd/kir/ckir_scan.hpp>   // B-cmp: build_scan (the CKIR device-wide prefix sum)
#include <crd/kir/ckir_mlp.hpp>    // v17 NRC: build_mlp_fwd_fp32 (the portable+bit-exact fused-MLP forward)
#include <crd/kir/ckir_svgf.hpp>   // B14-c: build_svgf_atrous (the SVGF edge-stopping denoiser)
#include <crd/kir/ckir_hair.hpp>   // B18-a: build_hair_bcsdf_kernel (the Chiang R/TT/TRT/TRRT hair/fur BCSDF)
#include <crd/kir/ckir_hair_geom.hpp> // B18-e: build_hair_filter_kernel (tangent-oriented compositing filter)
#include <crd/kir/ckir_hair_scatter.hpp> // B18-c: multiple-scattering tiers (moment LUT, dual scattering, DOM, volumetric MS)
#include <crd/kir/ckir_hlsl.hpp> // B-cmp: emit_compute_kernel_hlsl (the DX12 kernel emitter)
#include <crd/kir/ckir_visbuffer.hpp> // B4-vis: build_sw_raster_visbuffer (the compute software rasterizer)

#include <crd/math/cmath.hpp>        // Phase-1 FFT: host-side twiddle table
#include <ckir_kernel_dispatch.hpp> // B-cmp: the SHARED both-backend kernel dispatch + oracle-compare harness
#include <ckir_visbuffer_test.hpp>  // B4-vis: the SHARED software-rasterizer scene + oracle + mixed-dtype dispatch
#include <ckir_abuffer_test.hpp>    // B17-c: the SHARED exact-reference A-buffer OIT scene + oracle + 2-kernel dispatch

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace g = crd::gpu;

namespace
{
constexpr int kN = 4096;

const char* const kVecAddHlsl =
    "RWByteAddressBuffer A : register(u0);\n"
    "RWByteAddressBuffer B : register(u1);\n"
    "RWByteAddressBuffer O : register(u2);\n"
    "cbuffer C : register(b0) { uint n; };\n"
    "[numthreads(64,1,1)]\n"
    "void cs_main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint i = id.x;\n"
    "  if (i >= n) { return; }\n"
    "  float a = asfloat(A.Load(i * 4));\n"
    "  float b = asfloat(B.Load(i * 4));\n"
    "  O.Store(i * 4, asuint(a + b));\n"
    "}\n";
} // namespace

TEST_CASE("v17-i: D3D12 IComputeContext runs a kernel through the backend-agnostic surface", "[dx12][compute][gpu]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    const crd::u64 bytes = static_cast<crd::u64>(kN) * sizeof(float);

    float av[kN];
    float bv[kN];
    float expect[kN];
    for (int i = 0; i < kN; ++i)
    {
        av[i]     = (0.5F * static_cast<float>(i)) - 3.0F;
        bv[i]     = (2.0F * static_cast<float>(i)) + 1.0F;
        expect[i] = av[i] + bv[i];
    }

    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::StringView(kVecAddHlsl), 3, 4U);
    REQUIRE(pipe != nullptr);

    auto ga = ctx.create_buffer(bytes, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto gb = ctx.create_buffer(bytes, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto go = ctx.create_buffer(bytes, storage | transfer_src, g::ComputeMemory::GpuOnly);
    auto ua = ctx.create_buffer(bytes, transfer_src, g::ComputeMemory::CpuToGpu);
    auto ub = ctx.create_buffer(bytes, transfer_src, g::ComputeMemory::CpuToGpu);
    auto rb = ctx.create_buffer(bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    REQUIRE(ga != nullptr);
    REQUIRE(gb != nullptr);
    REQUIRE(go != nullptr);
    REQUIRE(ua != nullptr);
    REQUIRE(ub != nullptr);
    REQUIRE(rb != nullptr);

    { auto* p = static_cast<float*>(ua->map()); for (int i = 0; i < kN; ++i) { p[i] = av[i]; } ua->unmap(); }
    { auto* p = static_cast<float*>(ub->map()); for (int i = 0; i < kN; ++i) { p[i] = bv[i]; } ub->unmap(); }

    auto& rec = ctx.begin();
    rec.copy(*ua, *ga, 0U, 0U, bytes);
    rec.copy(*ub, *gb, 0U, 0U, bytes);
    g::ComputeBuffer* binds[] = {ga.get(), gb.get(), go.get()};
    crd::u32          n       = static_cast<crd::u32>(kN);
    rec.dispatch(*pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(binds, 3), &n, 4U, (kN + 63) / 64U, 1U, 1U);
    rec.barrier(*go, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*go, *rb, 0U, 0U, bytes);
    ctx.submit_and_wait();

    const auto* out  = static_cast<const float*>(rb->map());
    int         mism = 0;
    for (int i = 0; i < kN; ++i)
    {
        if (out[i] != expect[i]) { ++mism; }
    }
    rb->unmap();
    CHECK(mism == 0);
}

TEST_CASE("v17 NRC: CKIR fused-MLP FP32 forward DISPATCHES on DX12 == CPU oracle BIT-EXACT (the portable moat)",
          "[dx12][compute][gpu][kernel][mlp]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::MlpConfig mcfg;
    mcfg.batch_tile = 64;
    mcfg.warps      = 2;
    kir::KGraph       graph(&alloc);
    const kir::KEntry e     = kir::build_mlp_fwd_fp32(graph, mcfg);
    const int         wd    = mcfg.width;
    const int         batch = 64;
    const int         n_in  = batch * wd;
    const int         n_w   = mcfg.layers * wd * wd;

    crd::containers::Array<crd::f64> in64(&alloc);
    in64.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<crd::f64> w64(&alloc);
    w64.resize(static_cast<crd::usize>(n_w));
    crd::containers::Array<crd::f64> out64(&alloc);
    out64.resize(static_cast<crd::usize>(n_in));
    for (int i = 0; i < n_in; ++i) { in64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(0.2F * static_cast<float>((i * 7) % 13 - 6))); }
    for (int i = 0; i < n_w; ++i) { w64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(0.1F * static_cast<float>((i * 5) % 11 - 5))); }
    kir::KernelBuffer bufs[3] = {{in64.data(), n_in, 0, 0}, {w64.data(), n_w, 0, 1}, {out64.data(), n_in, 0, 2}};
    kir::eval_cpu_kernel(graph, e, bufs, 3, e.local_size[0], &alloc, static_cast<crd::u32>(batch));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 3, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> in32(&alloc);
    in32.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<float> w32(&alloc);
    w32.resize(static_cast<crd::usize>(n_w));
    crd::containers::Array<float> out32(&alloc);
    out32.resize(static_cast<crd::usize>(n_in));
    for (int i = 0; i < n_in; ++i) { in32[static_cast<crd::usize>(i)] = static_cast<float>(in64[static_cast<crd::usize>(i)]); }
    for (int i = 0; i < n_w; ++i) { w32[static_cast<crd::usize>(i)] = static_cast<float>(w64[static_cast<crd::usize>(i)]); }
    for (int i = 0; i < n_in; ++i) { out32[static_cast<crd::usize>(i)] = -1.0F; }
    float*    host[3] = {in32.data(), w32.data(), out32.data()};
    const int lens[3] = {n_in, n_w, n_in};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 3, static_cast<crd::u32>(batch));

    int bad = 0;
    for (int i = 0; i < n_in; ++i) { if (out32[static_cast<crd::usize>(i)] != static_cast<float>(out64[static_cast<crd::usize>(i)])) { ++bad; } }
    CHECK(bad == 0); // FP32 precise, no FMA ⇒ bit-IDENTICAL to Vulkan + the oracle
}

TEST_CASE("B14-c: CKIR SVGF a-trous denoiser DISPATCHES on DX12 == CPU oracle (ULP-tol, transcendental weights)",
          "[dx12][compute][gpu][kernel][svgf]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    kir::SvgfConfig   scfg;
    kir::KGraph       graph(&alloc);
    const kir::KEntry e  = kir::build_svgf_atrous(graph, scfg);
    const int         np = scfg.width * scfg.height;

    crd::containers::Array<crd::f64> color(&alloc);
    crd::containers::Array<crd::f64> gbuf(&alloc);
    crd::containers::Array<crd::f64> var(&alloc);
    crd::containers::Array<crd::f64> col_out(&alloc);
    crd::containers::Array<crd::f64> var_out(&alloc);
    color.resize(uz(np * 3));
    gbuf.resize(uz(np * 4));
    var.resize(uz(np));
    col_out.resize(uz(np * 3));
    var_out.resize(uz(np));
    crd::u32 s = 999U;
    auto rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < np; ++p)
    {
        for (int c = 0; c < 3; ++c) { color[uz(p * 3 + c)] = 0.4 + 0.3 * (rnd() - 0.5); }
        var[uz(p)]          = 0.05;
        gbuf[uz(p * 4 + 0)] = 1.0 + 0.1 * rnd();
        gbuf[uz(p * 4 + 1)] = 0.0;
        gbuf[uz(p * 4 + 2)] = 0.0;
        gbuf[uz(p * 4 + 3)] = 1.0;
    }
    kir::KernelBuffer bufs[5] = {{color.data(), np * 3, 0, 0}, {gbuf.data(), np * 4, 0, 1}, {var.data(), np, 0, 2},
                                 {col_out.data(), np * 3, 0, 3}, {var_out.data(), np, 0, 4}};
    kir::eval_cpu_kernel(graph, e, bufs, 5, e.local_size[0], &alloc, static_cast<crd::u32>(np / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 5, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> hc(&alloc);
    crd::containers::Array<float> hg(&alloc);
    crd::containers::Array<float> hv(&alloc);
    crd::containers::Array<float> hco(&alloc);
    crd::containers::Array<float> hvo(&alloc);
    hc.resize(uz(np * 3));
    hg.resize(uz(np * 4));
    hv.resize(uz(np));
    hco.resize(uz(np * 3));
    hvo.resize(uz(np));
    for (int i = 0; i < np * 3; ++i) { hc[uz(i)] = static_cast<float>(color[uz(i)]); }
    for (int i = 0; i < np * 4; ++i) { hg[uz(i)] = static_cast<float>(gbuf[uz(i)]); }
    for (int i = 0; i < np; ++i) { hv[uz(i)] = static_cast<float>(var[uz(i)]); }
    for (int i = 0; i < np * 3; ++i) { hco[uz(i)] = -9.0F; }
    float*    host[5] = {hc.data(), hg.data(), hv.data(), hco.data(), hvo.data()};
    const int lens[5] = {np * 3, np * 4, np, np * 3, np};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 5, static_cast<crd::u32>(np / 64));

    double maxrel = 0.0;
    for (int i = 0; i < np * 3; ++i)
    {
        const double ref = col_out[uz(i)];
        const double got = static_cast<double>(hco[uz(i)]);
        const double rel = std::fabs(got - ref) / (std::fabs(ref) + 1e-3);
        if (rel > maxrel) { maxrel = rel; }
    }
    std::printf("[DX12 SVGF a-trous 32x32] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 1e-4);
}

// B18-a: the Chiang R/TT/TRT/TRRT hair/fur BCSDF DISPATCHES on DX12 == CPU oracle (to-ULP). The DX12 mirror of the Vulkan hair
// gate — the SAME CKIR kernel → HLSL → DXIL. Transcendental-heavy (exp/log/asin/sinh), so a to-ULP tier like SVGF, not bit-exact.
TEST_CASE("B18-a: CKIR hair BCSDF (Chiang R/TT/TRT/TRRT) DISPATCHES on DX12 == CPU oracle (to-ULP)",
          "[dx12][compute][gpu][kernel][hair]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    const auto   uz  = [](int v) { return static_cast<crd::usize>(v); };
    const double kpi = kir::hair::kPi;

    kir::hair::HairKernelConfig hcfg; // η=1.55, βₘ=βₙ=0.3, α=2°
    const int                   n = 256;
    kir::KGraph                 graph(&alloc);
    const kir::KEntry           e = kir::hair::build_hair_bcsdf_kernel(graph, hcfg);

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 6));
    out.resize(uz(n));
    crd::u32 s   = 1337U; // same seed as the Vulkan gate ⇒ identical inputs ⇒ VK == DX12 comparable
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        in[uz(i * 6 + 0)] = (rnd() * 2.0 - 1.0) * 0.99; // sinθo
        in[uz(i * 6 + 1)] = (rnd() * 2.0 - 1.0) * kpi;  // φo
        in[uz(i * 6 + 2)] = (rnd() * 2.0 - 1.0) * 0.99; // sinθi
        in[uz(i * 6 + 3)] = (rnd() * 2.0 - 1.0) * kpi;  // φi
        in[uz(i * 6 + 4)] = rnd() * 2.0 - 1.0;          // h
        in[uz(i * 6 + 5)] = rnd() * 1.5;                // σₐ
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 6, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(graph, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(n * 6));
    h1.resize(uz(n));
    for (int i = 0; i < n * 6; ++i) { h0[uz(i)] = static_cast<float>(in[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    float*    host[2] = {h0.data(), h1.data()};
    const int lens[2] = {n * 6, n};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    double maxrel = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double gv = static_cast<double>(h1[uz(i)]);
        const double ov = out[uz(i)];
        const double ad = std::fabs(gv - ov);
        if (ad > maxabs) { maxabs = ad; }
        if (std::fabs(ov) > 1.0e-3) { const double rel = ad / std::fabs(ov); if (rel > maxrel) { maxrel = rel; } }
    }
    std::printf("[DX12 hair BCSDF] maxabs(GPU vs oracle) = %.3e  maxrel = %.3e\n", maxabs, maxrel);
    CHECK(maxabs < 1.0e-5);
    CHECK(maxrel < 3.0e-5);
}

// B18-b: the FUR BCSDF (hair + Yan-2017 double-cylinder MEDULLA scattered lobe) DISPATCHES on DX12 == CPU oracle to-ULP — the
// DX12 mirror of the Vulkan fur gate, SAME CKIR kernel → HLSL → DXIL, exercising the medulla branch (exp/cos/wrapped-Cauchy).
TEST_CASE("B18-b: CKIR fur BCSDF (medulla double-cylinder) DISPATCHES on DX12 == CPU oracle (to-ULP)",
          "[dx12][compute][gpu][kernel][hair][fur]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    const auto   uz  = [](int v) { return static_cast<crd::usize>(v); };
    const double kpi = kir::hair::kPi;

    kir::hair::HairKernelConfig hcfg;
    hcfg.fur_kappa  = 0.6;
    hcfg.fur_sigma  = 3.0;
    hcfg.fur_albedo = 0.7;
    hcfg.fur_g      = 0.3;
    const int         n = 256;
    kir::KGraph       graph(&alloc);
    const kir::KEntry e = kir::hair::build_hair_bcsdf_kernel(graph, hcfg);

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 6));
    out.resize(uz(n));
    crd::u32 s   = 2027U; // same seed as the Vulkan fur gate ⇒ comparable inputs
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        in[uz(i * 6 + 0)] = (rnd() * 2.0 - 1.0) * 0.99;
        in[uz(i * 6 + 1)] = (rnd() * 2.0 - 1.0) * kpi;
        in[uz(i * 6 + 2)] = (rnd() * 2.0 - 1.0) * 0.99;
        in[uz(i * 6 + 3)] = (rnd() * 2.0 - 1.0) * kpi;
        in[uz(i * 6 + 4)] = rnd() * 2.0 - 1.0;
        in[uz(i * 6 + 5)] = rnd() * 1.5;
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 6, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(graph, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(n * 6));
    h1.resize(uz(n));
    for (int i = 0; i < n * 6; ++i) { h0[uz(i)] = static_cast<float>(in[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    float*    host[2] = {h0.data(), h1.data()};
    const int lens[2] = {n * 6, n};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    double maxrel = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double gv = static_cast<double>(h1[uz(i)]);
        const double ov = out[uz(i)];
        const double ad = std::fabs(gv - ov);
        if (ad > maxabs) { maxabs = ad; }
        if (std::fabs(ov) > 1.0e-3) { const double rel = ad / std::fabs(ov); if (rel > maxrel) { maxrel = rel; } }
    }
    std::printf("[DX12 fur BCSDF] maxabs(GPU vs oracle) = %.3e  maxrel = %.3e\n", maxabs, maxrel);
    CHECK(maxabs < 1.0e-5);
    CHECK(maxrel < 3.0e-5);
}

// B18-c: the hair MULTIPLE-SCATTERING tiers DISPATCH on DX12 == CPU oracle — the DX12 mirror of the Vulkan scattering gate.
// Same CKIR graphs → HLSL → DXIL. Physics is gated CPU-side; this is portability.
TEST_CASE("B18-c: hair multiple-scattering tiers DISPATCH on DX12 == CPU oracle", "[dx12][compute][gpu][kernel][hair][scatter]")
{
    namespace kir = crd::kir;
    namespace hms = crd::kir::hairms;
    crd::memory::TlsfAllocator alloc(192U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    const auto both = [&](kir::KGraph& gg, const kir::KEntry& e, double** data, const int* lens, int nbuf, int check,
                          crd::u32 groups, const char* name) -> double {
        crd::containers::Array<double> snap(&alloc);
        int total = 0;
        for (int i = 0; i < nbuf; ++i) { total += lens[i]; }
        snap.resize(uz(total), 0.0);
        int off = 0;
        for (int i = 0; i < nbuf; ++i) { for (int j = 0; j < lens[i]; ++j) { snap[uz(off + j)] = data[i][j]; } off += lens[i]; }

        kir::KernelBuffer bufs[6];
        for (int i = 0; i < nbuf; ++i) { bufs[i] = {data[i], lens[i], 0U, static_cast<crd::u8>(i)}; }
        kir::eval_cpu_kernel(gg, e, bufs, nbuf, e.local_size[0], &alloc, groups);
        crd::containers::Array<double> ref(&alloc);
        ref.resize(uz(lens[check]), 0.0);
        for (int j = 0; j < lens[check]; ++j) { ref[uz(j)] = data[check][j]; }
        off = 0;
        for (int i = 0; i < nbuf; ++i) { for (int j = 0; j < lens[i]; ++j) { data[i][j] = snap[uz(off + j)]; } off += lens[i]; }

        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(gg, e, &alloc, kern));
        auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), static_cast<crd::u32>(nbuf), 0U);
        REQUIRE(pipe != nullptr);
        crd::containers::Array<float> host_store(&alloc);
        host_store.resize(uz(total), 0.0F);
        float* host[6];
        off = 0;
        for (int i = 0; i < nbuf; ++i)
        {
            host[i] = host_store.data() + off;
            for (int j = 0; j < lens[i]; ++j) { host[i][j] = static_cast<float>(data[i][j]); }
            off += lens[i];
        }
        crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, nbuf, groups);
        double worst = 0.0;
        for (int j = 0; j < lens[check]; ++j)
        {
            const double d = static_cast<double>(host[check][j]) - ref[uz(j)];
            const double a = d < 0.0 ? -d : d;
            if (a > worst) { worst = a; }
        }
        std::printf("[DX12 B18-c %s] maxabs(GPU vs oracle) = %.3e\n", name, worst);
        return worst;
    };

    { // moment LUT
        hms::HairScatterLutConfig lc;
        lc.n_theta_d = 64; lc.n_h = 2; lc.n_theta_o = 8; lc.n_phi_o = 16;
        kir::KGraph       gg(&alloc);
        const kir::KEntry e = hms::build_hair_scatter_lut_kernel(gg, lc);
        crd::containers::Array<double> out(&alloc);
        out.resize(uz(64 * hms::kLutStride), 0.0);
        double*   data[1] = {out.data()};
        const int lens[1] = {64 * hms::kLutStride};
        CHECK(both(gg, e, data, lens, 1, 0, 1U, "scatter_lut") < 1.0e-5);
    }
    { // volumetric multiple scattering
        hms::VolumeMsConfig vc;
        kir::KGraph         gg(&alloc);
        const kir::KEntry   e = hms::build_volume_ms_kernel(gg, vc);
        crd::containers::Array<double> in(&alloc), out(&alloc);
        in.resize(uz(64 * 5), 0.0);
        out.resize(uz(64 * 2), 0.0);
        for (int k = 0; k < 64; ++k)
        {
            const crd::usize o = uz(k * 5);
            in[o + 0U] = 2.0; in[o + 1U] = static_cast<double>(k) / 64.0; in[o + 2U] = 0.8;
            in[o + 3U] = 0.25; in[o + 4U] = 1.5;
        }
        double*   data[2] = {in.data(), out.data()};
        const int lens[2] = {64 * 5, 64 * 2};
        CHECK(both(gg, e, data, lens, 2, 1, 1U, "volume_ms") < 1.0e-5);
    }
    { // deep opacity map build
        hms::DomConfig dc;
        dc.layers = 4; dc.span = 4.0; dc.frags_per_px = 16;
        const int stride = 1 + dc.layers;
        crd::containers::Array<double> frags(&alloc), dom(&alloc);
        frags.resize(uz(64 * 16 * 2), 0.0);
        dom.resize(uz(64 * stride), 0.0);
        for (int p = 0; p < 64; ++p)
        {
            const double z0 = 1.0 + 0.05 * static_cast<double>(p);
            for (int f = 0; f < 16; ++f)
            {
                const crd::usize o = uz((p * 16 + f) * 2);
                frags[o + 0U] = z0 + 3.0 * ((static_cast<double>(f) + 0.5) / 16.0);
                frags[o + 1U] = 0.1;
            }
        }
        kir::KGraph       gg(&alloc);
        const kir::KEntry e = hms::build_dom_build_kernel(gg, dc);
        double*   data[2] = {frags.data(), dom.data()};
        const int lens[2] = {64 * 16 * 2, 64 * stride};
        CHECK(both(gg, e, data, lens, 2, 1, 1U, "dom_build") < 1.0e-5);
    }
}

// B18-e: the COMPOSITING filter (Lipp 2026) DISPATCHES on DX12 == CPU oracle — the DX12 mirror of the Vulkan filter gate.
// Same purpose: portability, plus the tail-lane BOUNDS GUARD that only a real rounded-up dispatch exercises. Worth mirroring
// specifically because HLSL COERCES types where GLSL rejects them, so a shape bug here can pass on one backend and fail on the
// other — the whole reason both gates exist.
TEST_CASE("B18-e: CKIR hair compositing filter DISPATCHES on DX12 == CPU oracle", "[dx12][compute][gpu][kernel][hair][filter]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    kir::hairgeom::HairFilterConfig fc; // 10x9 = 90 px over 2 groups of 64 ⇒ 38 tail lanes, deliberately out of range
    fc.width  = 10;
    fc.height = 9;
    const int np = fc.width * fc.height;

    kir::KGraph       gg(&alloc);
    const kir::KEntry e = kir::hairgeom::build_hair_filter_kernel(gg, fc);

    crd::containers::Array<double> col(&alloc), tan(&alloc), dep(&alloc), out(&alloc);
    col.resize(uz(np * 3), 0.0);
    tan.resize(uz(np * 2), 0.0);
    dep.resize(uz(np), 0.0);
    out.resize(uz(np * 4), 0.0);
    crd::u32   st  = 0x9E3779B9U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < np; ++i)
    {
        for (int c = 0; c < 3; ++c) { col[uz(i * 3 + c)] = rnd(); }
        const double a     = rnd() * 6.2831853;
        tan[uz(i * 2 + 0)] = crd::math::cos(a);
        tan[uz(i * 2 + 1)] = crd::math::sin(a);
        dep[uz(i)]         = 0.5 + 0.002 * rnd();
    }
    crd::containers::Array<double> snap(&alloc);
    snap.resize(uz(np * 6), 0.0);
    for (int j = 0; j < np * 3; ++j) { snap[uz(j)] = col[uz(j)]; }
    for (int j = 0; j < np * 2; ++j) { snap[uz(np * 3 + j)] = tan[uz(j)]; }
    for (int j = 0; j < np; ++j) { snap[uz(np * 5 + j)] = dep[uz(j)]; }

    const crd::u32    groups  = (static_cast<crd::u32>(np) + e.local_size[0] - 1U) / e.local_size[0];
    kir::KernelBuffer bufs[4] = {{col.data(), np * 3, 0U, 0U},
                                 {tan.data(), np * 2, 0U, 1U},
                                 {dep.data(), np, 0U, 2U},
                                 {out.data(), np * 4, 0U, 3U}};
    kir::eval_cpu_kernel(gg, e, bufs, 4, e.local_size[0], &alloc, groups);
    crd::containers::Array<double> ref(&alloc);
    ref.resize(uz(np * 4), 0.0);
    for (int j = 0; j < np * 4; ++j) { ref[uz(j)] = out[uz(j)]; }

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(gg, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 4U, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> host_store(&alloc);
    host_store.resize(uz(np * 10), 0.0F);
    float*    host[4] = {host_store.data(), host_store.data() + np * 3, host_store.data() + np * 5, host_store.data() + np * 6};
    const int lens[4] = {np * 3, np * 2, np, np * 4};
    for (int j = 0; j < np * 3; ++j) { host[0][j] = static_cast<float>(snap[uz(j)]); }
    for (int j = 0; j < np * 2; ++j) { host[1][j] = static_cast<float>(snap[uz(np * 3 + j)]); }
    for (int j = 0; j < np; ++j) { host[2][j] = static_cast<float>(snap[uz(np * 5 + j)]); }
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 4, groups);

    double worst = 0.0;
    for (int j = 0; j < np * 4; ++j)
    {
        const double d = static_cast<double>(host[3][j]) - ref[uz(j)];
        const double a = d < 0.0 ? -d : d;
        if (a > worst) { worst = a; }
    }
    std::printf("[DX12 B18-e filter] %dx%d over %u groups (%d tail lanes)  maxabs(GPU vs oracle) = %.3e\n",
                fc.width, fc.height, groups, static_cast<int>(groups * e.local_size[0]) - np, worst);
    CHECK(worst < 1.0e-5);
    for (int i = 0; i < np; ++i) { CHECK(host[3][i * 4 + 3] > 0.0F); }
}


// B18-b: the HUANG 2022 microfacet R lobe DISPATCHES on DX12 == CPU oracle to-ULP — the DX12 mirror of the Vulkan Huang gate.
TEST_CASE("B18-b: CKIR Huang microfacet R lobe DISPATCHES on DX12 == CPU oracle (to-ULP)",
          "[dx12][compute][gpu][kernel][hair][huang]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    const auto   uz  = [](int v) { return static_cast<crd::usize>(v); };
    const double kpi = kir::hair::kPi;

    kir::hair::HairKernelConfig hcfg;
    // HuangFull gates both halves in one dispatch: the analytic R closed form AND the TT/TRT Simpson + VNDF/refraction chain.
    hcfg.model      = kir::hair::HairModel::HuangFull;
    hcfg.huang_beta = 0.3;
    hcfg.simpson_n  = 12;
    const int         n = 256;
    kir::KGraph       graph(&alloc);
    const kir::KEntry e = kir::hair::build_hair_bcsdf_kernel(graph, hcfg);

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 6));
    out.resize(uz(n));
    crd::u32 s   = 91177U; // same seed as the Vulkan Huang gate
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        in[uz(i * 6 + 0)] = (rnd() * 2.0 - 1.0) * 0.93;
        in[uz(i * 6 + 1)] = (rnd() * 2.0 - 1.0) * kpi;
        in[uz(i * 6 + 2)] = (rnd() * 2.0 - 1.0) * 0.93;
        in[uz(i * 6 + 3)] = (rnd() * 2.0 - 1.0) * kpi;
        in[uz(i * 6 + 4)] = rnd() * 2.0 - 1.0;
        in[uz(i * 6 + 5)] = rnd() * 1.5;
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 6, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(graph, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(n * 6));
    h1.resize(uz(n));
    for (int i = 0; i < n * 6; ++i) { h0[uz(i)] = static_cast<float>(in[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    float*    host[2] = {h0.data(), h1.data()};
    const int lens[2] = {n * 6, n};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    double maxrel = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double gv = static_cast<double>(h1[uz(i)]);
        const double ov = out[uz(i)];
        const double ad = std::fabs(gv - ov);
        if (ad > maxabs) { maxabs = ad; }
        if (std::fabs(ov) > 1.0e-3) { const double rel = ad / std::fabs(ov); if (rel > maxrel) { maxrel = rel; } }
    }
    std::printf("[DX12 Huang full] maxabs(GPU vs oracle) = %.3e  maxrel = %.3e\n", maxabs, maxrel);
    // See the Vulkan Huang gate for the tolerance rationale: maxabs stays tight (1e-5); maxrel is looser than the hair/fur
    // gates because HuangFull is ~20x the transcendental depth plus a 13-term f32 Simpson accumulation.
    CHECK(maxabs < 1.0e-5);
    CHECK(maxrel < 5.0e-4);
}

TEST_CASE("v17 NRC: CKIR fused-MLP BACKWARD (dz chain + DETERMINISTIC dW) DISPATCHES on DX12 == oracle bit-exact",
          "[dx12][compute][gpu][kernel][mlp]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::MlpConfig mcfg;
    mcfg.batch_tile = 64;
    mcfg.warps      = 2;
    const int wd    = mcfg.width;
    const int nl    = mcfg.layers;
    const int batch = 16;
    const int bw    = batch * wd;
    const int n_a   = (nl + 1) * bw;
    const int n_w   = nl * wd * wd;
    const int n_dz  = nl * bw;
    const int n_dw  = nl * wd * wd;

    crd::containers::Array<float> a_all(&alloc);
    a_all.resize(static_cast<crd::usize>(n_a));
    crd::containers::Array<float> w_f(&alloc);
    w_f.resize(static_cast<crd::usize>(n_w));
    for (int i = 0; i < n_w; ++i) { w_f[static_cast<crd::usize>(i)] = 0.1F * static_cast<float>((i * 5) % 11 - 5); }
    for (int r = 0; r < batch; ++r)
    {
        for (int c = 0; c < wd; ++c)
        {
            const int ai                       = r * wd + c;
            a_all[static_cast<crd::usize>(ai)] = 0.2F * static_cast<float>(ai % 13 - 6);
        }
        for (int l = 0; l < nl; ++l)
        {
            for (int n = 0; n < wd; ++n)
            {
                float z = 0.0F;
                for (int k = 0; k < wd; ++k)
                {
                    const int ci = l * bw + r * wd + k;
                    const int wi = l * wd * wd + k * wd + n;
                    z += a_all[static_cast<crd::usize>(ci)] * w_f[static_cast<crd::usize>(wi)];
                }
                const int oi                       = (l + 1) * bw + r * wd + n;
                a_all[static_cast<crd::usize>(oi)] = (l + 1 < nl && z < 0.0F) ? 0.0F : z;
            }
        }
    }
    crd::containers::Array<float> gout(&alloc);
    gout.resize(static_cast<crd::usize>(bw));
    for (int i = 0; i < bw; ++i)
    {
        const int gi                     = nl * bw + i;
        gout[static_cast<crd::usize>(i)] = a_all[static_cast<crd::usize>(gi)];
    }
    crd::containers::Array<float> ref_dz(&alloc);
    ref_dz.resize(static_cast<crd::usize>(n_dz));
    crd::containers::Array<float> ref_dw(&alloc);
    ref_dw.resize(static_cast<crd::usize>(n_dw));
    crd::containers::Array<float> gs(&alloc);
    gs.resize(static_cast<crd::usize>(wd));
    crd::containers::Array<float> ngs(&alloc);
    ngs.resize(static_cast<crd::usize>(wd));
    kir::mlp_backward_ref(mcfg, a_all.data(), w_f.data(), gout.data(), batch, ref_dz.data(), ref_dw.data(), gs.data(), ngs.data());

    kir::KGraph       g_a(&alloc);
    const kir::KEntry e_a = kir::build_mlp_bwd_dz(g_a, mcfg, batch);
    kir::GlslKernel   k_a(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(g_a, e_a, &alloc, k_a));
    auto pipe_a = ctx.create_pipeline_from_hlsl(crd::containers::to_view(k_a.source), 4, 0U);
    REQUIRE(pipe_a != nullptr);
    crd::containers::Array<float> dz(&alloc);
    dz.resize(static_cast<crd::usize>(n_dz));
    for (int i = 0; i < n_dz; ++i) { dz[static_cast<crd::usize>(i)] = -7.0F; }
    float*    host_a[4] = {a_all.data(), w_f.data(), gout.data(), dz.data()};
    const int lens_a[4] = {n_a, n_w, bw, n_dz};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe_a, host_a, lens_a, 4, static_cast<crd::u32>(batch));
    int bad_dz = 0;
    for (int i = 0; i < n_dz; ++i) { if (dz[static_cast<crd::usize>(i)] != ref_dz[static_cast<crd::usize>(i)]) { ++bad_dz; } }
    CHECK(bad_dz == 0);

    kir::KGraph       g_b(&alloc);
    const kir::KEntry e_b = kir::build_mlp_bwd_dw(g_b, mcfg, batch);
    kir::GlslKernel   k_b(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(g_b, e_b, &alloc, k_b));
    auto pipe_b = ctx.create_pipeline_from_hlsl(crd::containers::to_view(k_b.source), 3, 0U);
    REQUIRE(pipe_b != nullptr);
    crd::containers::Array<float> dw(&alloc);
    dw.resize(static_cast<crd::usize>(n_dw));
    for (int i = 0; i < n_dw; ++i) { dw[static_cast<crd::usize>(i)] = -7.0F; }
    float*    host_b[3] = {a_all.data(), dz.data(), dw.data()};
    const int lens_b[3] = {n_a, n_dz, n_dw};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe_b, host_b, lens_b, 3, static_cast<crd::u32>(nl * wd));
    int bad_dw = 0;
    for (int i = 0; i < n_dw; ++i) { if (dw[static_cast<crd::usize>(i)] != ref_dw[static_cast<crd::usize>(i)]) { ++bad_dw; } }
    CHECK(bad_dw == 0); // bit-IDENTICAL to Vulkan + the oracle (FP32 precise, deterministic ascending reduction)
}

TEST_CASE("B-cmp: CKIR compute KERNEL (shared memory + barriers) DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph       graph(&alloc);
    constexpr int     ls = 256;
    const kir::KEntry e  = crd::kir_test::build_reverse_kernel(graph, ls);

    // 1) the CPU ORACLE (f64 buffers, F32-rounded ops) — the bit-exact reference (SAME graph, SAME oracle as the Vulkan test).
    crd::f64 in64[ls];
    crd::f64 out64[ls];
    for (int i = 0; i < ls; ++i) { in64[i] = 1.0 + 3.0 * static_cast<crd::f64>(i); out64[i] = -1.0; } // exact in f32
    kir::KernelBuffer bufs[2] = {{in64, ls, 0, 0}, {out64, ls, 0, 1}};
    kir::eval_cpu_kernel(graph, e, bufs, 2, static_cast<crd::u32>(ls), &alloc);

    // 2) emit kernel HLSL → DXIL → pipeline (2 raw-UAV bindings, no push).
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 2, 0U);
    REQUIRE(pipe != nullptr);

    // 3) dispatch ONE workgroup on the portable surface, read back.
    float in32[ls];
    float out32[ls];
    for (int i = 0; i < ls; ++i) { in32[i] = static_cast<float>(in64[i]); out32[i] = -1.0F; }
    float*    host[2] = {in32, out32};
    const int lens[2] = {ls, ls};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 2, 1U);

    // 4) GPU == oracle, bit-for-bit (reverse is pure data movement ⇒ exact on every vendor + identical to Vulkan).
    int bad = 0;
    for (int i = 0; i < ls; ++i) { if (out32[i] != static_cast<float>(out64[i])) { ++bad; } }
    CHECK(bad == 0);
    CHECK(out32[0] == static_cast<float>(in64[ls - 1])); // spot-check the reversal actually happened
    CHECK(out32[ls - 1] == static_cast<float>(in64[0]));
}

TEST_CASE("B-cmp: CKIR TRANSPOSE kernel (For loops + barrier + cross-thread) DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph       graph(&alloc);
    constexpr int     t  = 8;
    constexpr int     nn = t * t;
    const kir::KEntry e  = crd::kir_test::build_transpose_kernel(graph, t);

    crd::f64 in64[nn];
    crd::f64 out64[nn];
    for (int i = 0; i < nn; ++i) { in64[i] = static_cast<crd::f64>(i); out64[i] = -1.0; }
    kir::KernelBuffer bufs[2] = {{in64, nn, 0, 0}, {out64, nn, 0, 1}};
    kir::eval_cpu_kernel(graph, e, bufs, 2, static_cast<crd::u32>(t), &alloc);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 2, 0U);
    REQUIRE(pipe != nullptr);

    float in32[nn];
    float out32[nn];
    for (int i = 0; i < nn; ++i) { in32[i] = static_cast<float>(in64[i]); out32[i] = -1.0F; }
    float*    host[2] = {in32, out32};
    const int lens[2] = {nn, nn};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 2, 1U);

    int bad = 0;
    for (int i = 0; i < nn; ++i) { if (out32[i] != static_cast<float>(out64[i])) { ++bad; } }
    CHECK(bad == 0);
    CHECK(out32[1] == static_cast<float>(in64[t])); // out[0][1] == in[1][0] — the transpose actually happened
}

// B4-vis: the NANITE software rasterizer (atomicMin visibility buffer) DISPATCHES on DX12 == CPU oracle bit-exact. One
// thread per triangle rasterizes into a per-pixel (depth<<idBits)|triangleId u32; the nearer centre triangle wins the
// overlap. Proves BufferAtomicMin + edge-function coverage + barycentric depth lower to DXIL and agree with the oracle.
TEST_CASE("B4-vis: CKIR software rasterizer (atomicMin visibility buffer) DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][visbuffer]")
{
    namespace kir = crd::kir;
    namespace vb  = crd::kir::visbuffer;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    const crd::kir_test::SwRasterScene scene = crd::kir_test::make_sw_raster_scene();
    kir::KGraph                        graph(&alloc);
    const kir::KEntry                  e = vb::build_sw_raster_visbuffer(graph, scene.cfg);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, e, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 3, 0U); // 3 UAV bindings, no push
    REQUIRE(pipe != nullptr);

    crd::containers::Array<crd::u32> vis_cpu(&alloc);
    crd::containers::Array<crd::u32> vis_gpu(&alloc);
    crd::kir_test::sw_raster_oracle(graph, e, scene, alloc, vis_cpu);
    crd::kir_test::dispatch_visraster(ctx, *pipe, scene, vis_gpu);

    const int npix = static_cast<int>(scene.cfg.width * scene.cfg.height);
    int       bad  = 0;
    for (int i = 0; i < npix; ++i) { if (vis_gpu[static_cast<crd::usize>(i)] != vis_cpu[static_cast<crd::usize>(i)]) { ++bad; } }
    CHECK(bad == 0); // BIT-EXACT: the GPU visibility keys equal the CPU oracle's (atomicMin is order-independent)

    const crd::u32 w  = scene.cfg.width;
    const crd::u32 ib = scene.cfg.id_bits;
    const auto     at = [&](crd::u32 x, crd::u32 y) { return vis_gpu[static_cast<crd::usize>(y) * w + x]; };
    CHECK(vb::vis_id(at(16U, 16U), ib) == 2U);   // centre: the NEAR triangle (id 2) wins the depth resolve
    CHECK(vb::vis_id(at(10U, 18U), ib) == 1U);   // upper-left half of the quad → triangle 1
    CHECK(vb::vis_id(at(20U, 10U), ib) == 0U);   // lower-right half of the quad → triangle 0
    CHECK(at(2U, 2U) == vb::kVisEmptyKey);       // outside the geometry → still the empty key
    CHECK(vb::vis_depth(at(16U, 16U), ib) < vb::vis_depth(at(20U, 10U), ib)); // centre nearer than the quad
}

// B4-vis-2: DEFERRED ATTRIBUTE INTERPOLATION SHADE (DAIS) DISPATCHES on DX12 == CPU oracle bit-exact. Reads the visibility
// buffer, fetches the triangle's 3 verts, reconstructs PERSPECTIVE-CORRECT barycentrics + interpolates a per-vertex attribute
// once per visible pixel. The scene's distinct clip-w makes perspective correction non-trivial: at the screen centroid the
// perspective-correct value (~8) is far below the naive barycentric average (14) — the test proves the /w correction ran.
TEST_CASE("B4-vis-2: CKIR deferred attribute shade (DAIS) DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][visbuffer][dais]")
{
    namespace kir = crd::kir;
    namespace vb  = crd::kir::visbuffer;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    const crd::kir_test::DaisScene   scene = crd::kir_test::make_dais_scene();
    crd::containers::Array<crd::u32> vis(&alloc);
    crd::kir_test::dais_make_vis(scene, alloc, vis); // CPU rasterize the perspective triangle → visibility keys

    kir::KGraph       dg(&alloc);
    const kir::KEntry de = vb::build_deferred_attr_shade(dg, scene.shade_cfg);
    kir::GlslKernel   kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(dg, de, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 5, 0U); // 5 UAV bindings, no push
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> shade_cpu(&alloc);
    crd::containers::Array<float> shade_gpu(&alloc);
    crd::kir_test::dais_oracle(dg, de, scene, vis, alloc, shade_cpu);
    crd::kir_test::dispatch_dais(ctx, *pipe, scene, vis, shade_gpu);

    const crd::u32 w    = scene.shade_cfg.width;
    const int      npix = static_cast<int>(w * scene.shade_cfg.height);
    // The VISIBILITY is bit-exact (B4-vis-1); the deferred SHADE has one data-dependent GPU divide (num/denom) which f32
    // hardware does not correctly-round (Vulkan/DX12 ~2.5 ULP), so the reconstruction matches the oracle to a TIGHT ULP
    // tolerance, not bit-for-bit — the same status as the ocean spectrum's transcendentals. All other ops are exact mul/add.
    const auto absf = [](float v) { return v < 0.0F ? -v : v; };
    float      max_rel = 0.0F;
    for (int i = 0; i < npix; ++i)
    {
        const float gp  = shade_gpu[static_cast<crd::usize>(i)];
        const float cp  = shade_cpu[static_cast<crd::usize>(i)];
        const float acp = absf(cp) > 1.0e-6F ? absf(cp) : 1.0e-6F;
        const float rel = absf(gp - cp) / acp;
        if (rel > max_rel) { max_rel = rel; }
    }
    WARN("[DAIS dx12] max relative error (GPU vs oracle) = " << max_rel);
    CHECK(max_rel < 1.0e-6F); // ≈ a few f32 ULP — the single perspective-normalize divide's hardware imprecision

    int covered  = 0;
    int outrange = 0;
    for (int i = 0; i < npix; ++i)
    {
        const float v = shade_gpu[static_cast<crd::usize>(i)];
        if (v != 0.0F)
        {
            ++covered;
            if (v < 1.9F || v > 32.1F) { ++outrange; } // a covered pixel is a convex blend of the {2,8,32} attributes
        }
    }
    CHECK(covered > 100);  // the perspective triangle rasterized + shaded a good fraction of the 32x32
    CHECK(outrange == 0);  // every covered pixel is within [min,max] attribute — a valid partition-of-unity blend
    CHECK(shade_gpu[2U * w + 2U] == 0.0F); // outside the triangle → the pre-cleared background
    const float centre = shade_gpu[13U * w + 16U]; // ≈ the screen centroid (equal screen barycentrics)
    CHECK(centre > 5.0F);
    CHECK(centre < 11.0F); // perspective-correct (~8), decisively below the naive average (14) — the /w correction is active
}

// B4-vis-3: the HZB two-pass OCCLUSION CULL DISPATCHES on DX12 == CPU oracle bit-exact. Builds a max-depth mip pyramid (one
// downsample dispatch per level) then tests cluster AABBs against it. MAX + compare are order-independent ⇒ bit-exact. Proves
// the GPU-driven culling that makes a Nanite pipeline scale: a cluster behind the near wall is culled; open / in-front stay.
TEST_CASE("B4-vis-3: CKIR HZB two-pass occlusion cull DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][visbuffer][hzb]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    const crd::kir_test::HzbScene scene = crd::kir_test::make_hzb_scene();
    bool                          emit_ok = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_hlsl(gr, en, &alloc, kern)) { emit_ok = false; }
        return ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbufs, 0U);
    };
    crd::containers::Array<crd::u32> vis_cpu(&alloc);
    crd::containers::Array<crd::u32> vis_gpu(&alloc);
    crd::kir_test::hzb_cull_oracle(scene, alloc, vis_cpu);
    crd::kir_test::hzb_cull_dispatch(ctx, make_pipe, scene, alloc, vis_gpu);
    REQUIRE(emit_ok); // every HZB / cull kernel lowered to DXIL

    for (int c = 0; c < crd::kir_test::HzbScene::n_clusters; ++c)
    {
        CHECK(vis_gpu[static_cast<crd::usize>(c)] == vis_cpu[static_cast<crd::usize>(c)]);       // BIT-EXACT vs oracle
        CHECK(vis_gpu[static_cast<crd::usize>(c)] == scene.expected[static_cast<crd::usize>(c)]); // analytic: cull=0, visible=1
    }
}

// D-007 B17-c: the EXACT-REFERENCE A-buffer OIT on DX12 — the SAME two CKIR compute kernels (deferred store + per-pixel sort
// + exact front-to-back composite) the Vulkan test runs. Pure f32 mul/add/sub on a deterministic sorted order ⇒ device output
// BIT-EXACT vs `eval_cpu_kernel` AND DX12 == Vulkan. The ground truth the approximate WBOIT/MBOIT tiers are measured against.
TEST_CASE("D-007 B17-c: exact-reference A-buffer OIT on DX12 (deferred store + per-pixel sort + composite, bit-exact)",
          "[dx12][compute][gpu][oit][kernel]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 4U;
    acfg.local_size = 64U;
    const auto scene = crd::gputest::make_oit_scene();
    acfg.bg[0]       = scene.background[0];
    acfg.bg[1]       = scene.background[1];
    acfg.bg[2]       = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_hlsl(gr, en, &alloc, kern)) { emit_ok = false; }
        return ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> cpu(&alloc);
    crd::containers::Array<float>    gpu_out(&alloc);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, cpu);
    crd::kir_test::abuffer_dispatch(ctx, make_pipe, acfg, scene, alloc, gpu_out);
    REQUIRE(emit_ok);
    REQUIRE(gpu_out.size() == cpu.size());

    double worst = 0.0;
    for (crd::usize i = 0; i < gpu_out.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(gpu_out[i]) - cpu[i]);
        if (d > worst) { worst = d; }
    }
    INFO("A-buffer exact-composite worst |GPU - oracle| = " << worst);
    CHECK(worst == 0.0); // deterministic sorted order + pure f32 mul/add/sub ⇒ BIT-EXACT
    CHECK(std::fabs(cpu[0] - static_cast<double>(scene.background[0])) > 0.05); // transparency actually composited
}

// D-007 B17-c (scalable): the ATOMIC LINKED-LIST A-buffer on DX12 — the deployable fragment capture enabled by NEW value-
// returning atomics in CKIR, lowered to HLSL `RWByteAddressBuffer.InterlockedAdd(off, 1, orig)` (node allocator) +
// `.InterlockedExchange(off, slot, orig)` (list push, out-param original — HLSL's form of a value-returning atomic). Fragments
// race into per-pixel lists; the resolve walks + sorts ⇒ the composite must match the static-slot EXACT reference BIT-FOR-BIT.
TEST_CASE("D-007 B17-c: scalable atomic linked-list A-buffer on DX12 (value-returning atomics == exact reference)",
          "[dx12][compute][gpu][oit][atomic]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 4U;
    acfg.local_size = 64U;
    const auto scene = crd::gputest::make_oit_scene();
    acfg.bg[0]       = scene.background[0];
    acfg.bg[1]       = scene.background[1];
    acfg.bg[2]       = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_hlsl(gr, en, &alloc, kern)) { emit_ok = false; }
        return ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::containers::Array<float>    gpu_out(&alloc);
    crd::kir_test::abuffer_atomic_dispatch(ctx, make_pipe, acfg, scene, alloc, gpu_out);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu); // the static-slot exact reference
    REQUIRE(emit_ok);
    REQUIRE(gpu_out.size() == exact_cpu.size());

    double worst = 0.0;
    for (crd::usize i = 0; i < gpu_out.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(gpu_out[i]) - exact_cpu[i]);
        if (d > worst) { worst = d; }
    }
    INFO("atomic A-buffer vs static-slot exact reference: worst |Delta| = " << worst);
    CHECK(worst == 0.0); // dynamic atomic capture + sort == the exact composite, bit-for-bit (DX12 == Vulkan)
    CHECK(std::fabs(exact_cpu[0] - static_cast<double>(scene.background[0])) > 0.05); // transparency actually composited
}

// D-007 B17-c (scalable): STOCHASTIC TRANSPARENCY on DX12 (Enderton 2010) — the SAME single CKIR kernel Vulkan runs. A
// deterministic integer hash (u32 avalanche, lowered to HLSL) drives the screen-door coverage; the u32 arithmetic wraps
// mod 2^32 identically on GPU and in the CPU oracle (the oracle's integer path was made 32-bit-exact for this), so the
// "random" result is BIT-EXACT DX12 == oracle == Vulkan — a portable, reproducible stochastic tier (consistent TAA history).
TEST_CASE("D-007 B17-c: stochastic transparency on DX12 (deterministic-hash coverage; unbiased == exact A-buffer)",
          "[dx12][compute][gpu][oit][stochastic]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 4U;
    acfg.local_size = 64U;
    acfg.samples    = 32U;
    const auto scene = crd::gputest::make_oit_scene();
    acfg.bg[0]       = scene.background[0];
    acfg.bg[1]       = scene.background[1];
    acfg.bg[2]       = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_hlsl(gr, en, &alloc, kern)) { emit_ok = false; }
        return ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> st_cpu(&alloc);
    crd::containers::Array<float>    st_gpu(&alloc);
    crd::kir_test::stochastic_oracle(acfg, scene, alloc, st_cpu);
    crd::kir_test::stochastic_dispatch(ctx, make_pipe, acfg, scene, alloc, st_gpu);
    REQUIRE(emit_ok);
    REQUIRE(st_gpu.size() == st_cpu.size());
    double worst = 0.0;
    for (crd::usize i = 0; i < st_gpu.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(st_gpu[i]) - st_cpu[i]);
        if (d > worst) { worst = d; }
    }
    INFO("stochastic GPU vs oracle worst |Delta| = " << worst);
    CHECK(worst == 0.0); // deterministic hash + 32-bit-exact oracle ⇒ bit-identical DX12 == oracle == Vulkan

    // UNBIASED: the W*H*32-sample pixel-average lands on the exact `over` composite (measured on the bit-identical oracle).
    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu);
    const crd::u32 wh      = acfg.width * acfg.height;
    double         mean[3] = {0.0, 0.0, 0.0};
    for (crd::u32 p = 0; p < wh; ++p)
    {
        for (int ch = 0; ch < 3; ++ch)
        { mean[ch] += st_cpu[static_cast<crd::usize>(p) * 3U + static_cast<crd::usize>(ch)]; }
    }
    double bias = 0.0;
    for (int ch = 0; ch < 3; ++ch)
    {
        const double b = std::fabs(mean[ch] / static_cast<double>(wh) - exact_cpu[static_cast<crd::usize>(ch)]);
        if (b > bias) { bias = b; }
    }
    INFO("stochastic bias(S=32) vs exact = " << bias);
    CHECK(bias < 0.02);                                                                // unbiased estimator of the exact over
    CHECK(std::fabs(exact_cpu[0] - static_cast<double>(scene.background[0])) > 0.05); // transparency actually composited
}

// D-007 B17-b: MOMENT-BASED OIT (MBOIT, Münstermann 2018) on DX12 — the SAME CKIR compute kernels the Vulkan test runs
// (absorbance-weighted 4-power-moment generation + the Peters-Klein Hamburger reconstruction, from the shared deferred store).
// 4 power moments resolve 2 depth masses EXACTLY ⇒ MBOIT is bit-exact at 2-layer complexity, beating WBOIT; DX12 == Vulkan.
TEST_CASE("D-007 B17-b: moment-based OIT (MBOIT) on DX12 (4-power-moment reconstruction — exact depth ordering, beats WBOIT)",
          "[dx12][compute][gpu][oit][kernel]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 2U;
    acfg.local_size = 64U;
    crd::gputest::WboitScene scene{};
    scene.count = 2U;
    scene.background[0] = 0.05F; scene.background[1] = 0.06F; scene.background[2] = 0.08F;
    scene.color[0][0] = 0.90F; scene.color[0][1] = 0.15F; scene.color[0][2] = 0.10F; scene.alpha[0] = 0.50F; scene.depth[0] = 0.25F;
    scene.color[1][0] = 0.10F; scene.color[1][1] = 0.20F; scene.color[1][2] = 0.90F; scene.alpha[1] = 0.50F; scene.depth[1] = 0.70F;
    acfg.bg[0] = scene.background[0];
    acfg.bg[1] = scene.background[1];
    acfg.bg[2] = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_hlsl(gr, en, &alloc, kern)) { emit_ok = false; }
        return ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> mb_cpu(&alloc);
    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::containers::Array<float>    mb_gpu(&alloc);
    crd::kir_test::mboit_oracle(acfg, scene, alloc, mb_cpu);
    crd::kir_test::mboit_dispatch(ctx, make_pipe, acfg, scene, alloc, mb_gpu);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu);
    REQUIRE(emit_ok);
    REQUIRE(mb_gpu.size() == mb_cpu.size());

    double worst_ulp = 0.0;
    for (crd::usize i = 0; i < mb_gpu.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(mb_gpu[i]) - mb_cpu[i]);
        if (d > worst_ulp) { worst_ulp = d; }
    }
    INFO("MBOIT GPU vs oracle worst |Δ| = " << worst_ulp);
    CHECK(worst_ulp < 1.0e-5);

    const auto q = [](double v) {
        double c = v;
        if (c < 0.0) { c = 0.0; }
        else if (c > 1.0) { c = 1.0; }
        return static_cast<int>(std::lround(c * 255.0));
    };
    int        mboit_err = 0;
    for (int ch = 0; ch < 3; ++ch)
    {
        const int d = std::abs(q(mb_cpu[static_cast<crd::usize>(ch)]) - q(exact_cpu[static_cast<crd::usize>(ch)]));
        if (d > mboit_err) { mboit_err = d; }
    }
    const crd::u32 wboit_err = crd::gputest::rgba8_max_channel_diff(crd::gputest::wboit_oracle_pixel(scene),
                                                                    crd::gputest::oit_exact_composite_rgba8(scene));
    INFO("2-layer: MBOIT vs exact = " << mboit_err << " LSB · WBOIT vs exact = " << wboit_err << " LSB");
    CHECK(mboit_err <= 1);
    CHECK(static_cast<crd::u32>(mboit_err) < wboit_err);
}

// D-007 B17-b (extension): 6-POWER-MOMENT MBOIT on DX12 — the hero tier lifted to 3-mass depth complexity (larger 4x4 Hankel
// Cholesky + cubic root-solve + Gauss-Radau form factor). Same CKIR kernel as Vulkan; 6 moments resolve 3 depth masses, so
// MBOIT lands ~1 LSB from exact and DECISIVELY beats WBOIT (18 LSB) at 3 layers. DX12 == Vulkan.
TEST_CASE("D-007 B17-b: 6-moment MBOIT on DX12 (larger Cholesky + cubic — 3 masses, beats WBOIT)",
          "[dx12][compute][gpu][oit][kernel]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 3U;
    acfg.local_size = 64U;
    crd::gputest::WboitScene scene{};
    scene.count = 3U;
    scene.background[0] = 0.05F; scene.background[1] = 0.06F; scene.background[2] = 0.08F;
    scene.color[0][0] = 0.92F; scene.color[0][1] = 0.10F; scene.color[0][2] = 0.10F; scene.alpha[0] = 0.60F; scene.depth[0] = 0.20F;
    scene.color[1][0] = 0.10F; scene.color[1][1] = 0.90F; scene.color[1][2] = 0.12F; scene.alpha[1] = 0.50F; scene.depth[1] = 0.50F;
    scene.color[2][0] = 0.10F; scene.color[2][1] = 0.12F; scene.color[2][2] = 0.92F; scene.alpha[2] = 0.70F; scene.depth[2] = 0.80F;
    acfg.bg[0] = scene.background[0];
    acfg.bg[1] = scene.background[1];
    acfg.bg[2] = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_hlsl(gr, en, &alloc, kern)) { emit_ok = false; }
        return ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> mb_cpu(&alloc);
    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::containers::Array<float>    mb_gpu(&alloc);
    crd::kir_test::mboit6_oracle(acfg, scene, alloc, mb_cpu);
    crd::kir_test::mboit6_dispatch(ctx, make_pipe, acfg, scene, alloc, mb_gpu);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu);
    REQUIRE(emit_ok);
    REQUIRE(mb_gpu.size() == mb_cpu.size());

    double worst_ulp = 0.0;
    for (crd::usize i = 0; i < mb_gpu.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(mb_gpu[i]) - mb_cpu[i]);
        if (d > worst_ulp) { worst_ulp = d; }
    }
    INFO("MBOIT6 GPU vs oracle worst |Δ| = " << worst_ulp);
    CHECK(worst_ulp < 5.0e-3);

    const auto q = [](double v) {
        double c = v;
        if (c < 0.0) { c = 0.0; }
        else if (c > 1.0) { c = 1.0; }
        return static_cast<int>(std::lround(c * 255.0));
    };
    int        mboit_err = 0;
    for (int ch = 0; ch < 3; ++ch)
    {
        const int d = std::abs(q(mb_cpu[static_cast<crd::usize>(ch)]) - q(exact_cpu[static_cast<crd::usize>(ch)]));
        if (d > mboit_err) { mboit_err = d; }
    }
    const crd::u32 wboit_err = crd::gputest::rgba8_max_channel_diff(crd::gputest::wboit_oracle_pixel(scene),
                                                                    crd::gputest::oit_exact_composite_rgba8(scene));
    INFO("3-layer: MBOIT6 vs exact = " << mboit_err << " LSB · WBOIT vs exact = " << wboit_err << " LSB");
    CHECK(mboit_err <= 3);
    CHECK(static_cast<crd::u32>(mboit_err) + 8U < wboit_err);
}

TEST_CASE("B-cmp Phase 1: CKIR radix-2 Stockham FFT DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][fft]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph          graph(&alloc);
    constexpr int        n    = 64;
    constexpr int        half = n / 2;
    const kir::Fft1dPlan plan = kir::build_fft1d_radix2(graph, n, false);

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    crd::f64           twr[half];
    crd::f64           twi[half];
    for (int k = 0; k < half; ++k)
    {
        const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[k]           = static_cast<crd::f64>(static_cast<float>(crd::math::cos(a)));
        twi[k]           = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(a)));
    }
    crd::f64 ir[n];
    crd::f64 ii[n];
    crd::f64 orr[n];
    crd::f64 oi[n];
    for (int i = 0; i < n; ++i)
    {
        ir[i]  = static_cast<crd::f64>(static_cast<float>((i * 7 + 3) % 11 - 5));
        ii[i]  = static_cast<crd::f64>(static_cast<float>((i * 5 + 1) % 7 - 3));
        orr[i] = -99.0;
        oi[i]  = -99.0;
    }
    kir::KernelBuffer bufs[6] = {{ir, n, 0, 0},   {ii, n, 0, 1},   {twr, half, 0, 2},
                                 {twi, half, 0, 3}, {orr, n, 0, 4}, {oi, n, 0, 5}};
    kir::eval_cpu_kernel(graph, plan.entry, bufs, 6, static_cast<crd::u32>(half), &alloc);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(graph, plan.entry, &alloc, kern));
    auto pipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 6, 0U);
    REQUIRE(pipe != nullptr);

    float h_ir[n];
    float h_ii[n];
    float h_twr[half];
    float h_twi[half];
    float h_or[n];
    float h_oi[n];
    for (int i = 0; i < n; ++i) { h_ir[i] = static_cast<float>(ir[i]); h_ii[i] = static_cast<float>(ii[i]); h_or[i] = -99.0F; h_oi[i] = -99.0F; }
    for (int k = 0; k < half; ++k) { h_twr[k] = static_cast<float>(twr[k]); h_twi[k] = static_cast<float>(twi[k]); }
    float*    host[6] = {h_ir, h_ii, h_twr, h_twi, h_or, h_oi};
    const int lens[6] = {n, n, half, half, n, n};
    crd::kir_test::dispatch_kernel_1wg(ctx, *pipe, host, lens, 6, 1U);

    int bad = 0; // `precise` HLSL temps ⇒ BIT-EXACT vs the oracle (identical to the Vulkan FFT result)
    for (int k = 0; k < n; ++k) { if (h_or[k] != static_cast<float>(orr[k]) || h_oi[k] != static_cast<float>(oi[k])) { ++bad; } }
    CHECK(bad == 0);
}

// B-cmp Phase 2: the FULL 2-D FFT — the same 6-dispatch pipeline (row FFT -> transpose re,im -> col FFT -> transpose-back
// re,im) on DX12, BIT-FOR-BIT vs the CPU oracle AND (by construction) vs the Vulkan result: ONE CKIR graph, every backend.
TEST_CASE("B-cmp Phase 2: CKIR 2-D FFT (6-dispatch pipeline) DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][fft]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph          g0(&alloc); // one graph per unique entry (a CKIR emitter emits all of a graph's decls)
    kir::KGraph          g1(&alloc);
    kir::KGraph          g2(&alloc);
    kir::KGraph          g3(&alloc);
    kir::KGraph*         graphs[4] = {&g0, &g1, &g2, &g3};
    constexpr int        rr        = 64;
    constexpr int        cc        = 64;
    constexpr int        tile      = 16;
    const kir::Fft2dPlan plan      = kir::build_fft2d_c2c(graphs, rr, cc, false, tile);

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[16];
    float*    h32[16];
    for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = a64.data() + off[b]; h32[b] = a32.data() + off[b]; }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rr * cc; ++i)
    {
        h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
        h64[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 7 - 3);
    }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc); h64[plan.tw_col_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr); h64[plan.tw_row_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_row_im][k] = f32d(-crd::math::sin(a)); }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

    std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
    crd::gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        pipe_store[pi] = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(ctx, plan, pipes, h32);

    int badr = 0;
    int badi = 0;
    for (int i = 0; i < rr * cc; ++i)
    {
        if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
        if (h32[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
    }
    CHECK(badr == 0);
    CHECK(badi == 0);
}

// B16-a-2: the BATCHED strided inverse 2-D FFT (build_fft2d_c2c_batched — 2 dispatches: batched row IFFT -> batched strided+
// tiled column IFFT) the FFT-ocean rides, on DX12, BIT-FOR-BIT vs the CPU oracle across ALL batch images. DX12/HLSL coerces
// types (masking emitter bugs Vulkan catches), so running BOTH backends is the portability gate for the new radix-16 path.
TEST_CASE("B16-a-2: CKIR BATCHED strided inverse 2-D FFT DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][fft][ocean]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(96U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph          g0(&alloc);
    kir::KGraph          g1(&alloc);
    kir::KGraph*         graphs[2] = {&g0, &g1};
    constexpr int        n         = 64; // power of FOUR
    constexpr int        batch     = 4;
    constexpr int        tile_c    = 8;
    constexpr int        rc        = n * n;
    const kir::Fft2dPlan plan      = kir::build_fft2d_c2c_batched(graphs, n, n, batch, /*inverse=*/true, tile_c);

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[16];
    float*    h32[16];
    for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = a64.data() + off[b]; h32[b] = a32.data() + off[b]; }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rc * batch; ++i)
    {
        h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
        h64[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 13 - 6);
    }
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        h64[plan.tw_col_re][k] = f32d(crd::math::cos(a));
        h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a));
        h64[plan.tw_row_re][k] = h64[plan.tw_col_re][k];
        h64[plan.tw_row_im][k] = h64[plan.tw_col_im][k];
    }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

    std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
    crd::gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        pipe_store[pi] = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(ctx, plan, pipes, h32);

    int badr = 0;
    int badi = 0;
    for (int i = 0; i < rc * batch; ++i)
    {
        if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
        if (h32[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
    }
    CHECK(badr == 0);
    CHECK(badi == 0);
}

// B-cmp Phase 3: THE CRUSH — the FUSED 2-D FFT-convolution (7 dispatches) on DX12, BIT-FOR-BIT vs the CPU oracle AND (by
// construction) the Vulkan result. One CKIR graph set, every backend, identical bits — the mission's determinism, at 2-D scale.
TEST_CASE("B-cmp Phase 3: CKIR FUSED 2-D FFT-convolution DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][fft][conv]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(96U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph          g0(&alloc);
    kir::KGraph          g1(&alloc);
    kir::KGraph          g2(&alloc);
    kir::KGraph          g3(&alloc);
    kir::KGraph          g4(&alloc);
    kir::KGraph*         graphs[5] = {&g0, &g1, &g2, &g3, &g4};
    constexpr int        rr        = 64;
    constexpr int        cc        = 64;
    constexpr int        tile      = 16;
    const kir::Fft2dPlan plan      = kir::build_fft2d_convolution(graphs, rr, cc, tile);

    int off[20];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[20];
    float*    h32[20];
    for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = a64.data() + off[b]; h32[b] = a32.data() + off[b]; }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rr * cc; ++i)
    {
        h64[plan.in_re][i]   = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
        h64[plan.in_im][i]   = static_cast<crd::f64>((i * 5 + 1) % 7 - 3);
        h64[plan.filt_re][i] = f32d(static_cast<crd::f64>((i * 3 + 1) % 9 - 4) * 0.25);
        h64[plan.filt_im][i] = f32d(static_cast<crd::f64>((i * 2 + 5) % 7 - 3) * 0.25);
    }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc); h64[plan.tw_col_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr); h64[plan.tw_row_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_row_im][k] = f32d(-crd::math::sin(a)); }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

    std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
    crd::gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        pipe_store[pi] = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(ctx, plan, pipes, h32);

    int badr = 0;
    int badi = 0;
    for (int i = 0; i < rr * cc; ++i)
    {
        if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
        if (h32[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
    }
    CHECK(badr == 0);
    CHECK(badi == 0);
}

// R2C REAL 2-D FFT-convolution DISPATCHES bit-exact on DX12 (HLSL) vs the CPU oracle — the real-FFT crush path lowers to
// HLSL identically (Min/Select/If half-store + Hermitian-expand; the Materialize hoist that fixed the GLSL if-scope carries
// to every emitter). Bit-exact GPU==oracle with an arbitrary filter (correctness vs a true convolution is the [kir] oracle's job).
TEST_CASE("B-cmp: CKIR R2C REAL 2-D fused conv DISPATCHES on DX12 == CPU oracle bit-exact",
          "[dx12][compute][gpu][kernel][fft][conv]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(96U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph          g0(&alloc);
    kir::KGraph          g1(&alloc);
    kir::KGraph          g2(&alloc);
    kir::KGraph*         graphs[3] = {&g0, &g1, &g2};
    constexpr int        rr        = 64;
    constexpr int        cc        = 64;
    constexpr int        tc        = 4;
    const kir::Fft2dPlan plan      = kir::build_fft2d_convolution_r2c(graphs, rr, cc, tc, 1);
    const int            hw        = plan.buffers[static_cast<crd::usize>(plan.filt_re)].size / rr;

    int off[20];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[20];
    float*    h32[20];
    for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = a64.data() + off[b]; h32[b] = a32.data() + off[b]; }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rr * cc; ++i) { h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5); } // REAL image
    for (int i = 0; i < rr * hw; ++i) // arbitrary HALF filter (bit-exact GPU==oracle is filter-independent)
    {
        h64[plan.filt_re][i] = f32d(static_cast<crd::f64>((i * 3 + 1) % 9 - 4) * 0.25);
        h64[plan.filt_im][i] = f32d(static_cast<crd::f64>((i * 2 + 5) % 7 - 3) * 0.25);
    }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc); h64[plan.tw_col_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr); h64[plan.tw_row_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_row_im][k] = f32d(-crd::math::sin(a)); }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

    std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
    crd::gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        pipe_store[pi] = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(ctx, plan, pipes, h32);

    int badr = 0;
    for (int i = 0; i < rr * cc; ++i) { if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; } }
    CHECK(badr == 0);
}

// B-cmp: the CKIR device-wide REDUCTION dispatches bit-exact on DX12 (HLSL) vs the CPU oracle — the 2-pass plan lowers to
// HLSL identically (serial+tree combine, If-guarded shared writes). Sum bit-exact, max order-invariant.
TEST_CASE("B-cmp: CKIR device REDUCTION DISPATCHES on DX12 == CPU oracle bit-exact", "[dx12][compute][gpu][kernel][reduce]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    constexpr int n = 65536;
    crd::containers::Array<crd::f64> x64(&alloc);
    crd::containers::Array<float>    x32(&alloc);
    x64.resize(n); x32.resize(n);
    for (int i = 0; i < n; ++i) { x64[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 31 + 5) % 251 - 125); x32[static_cast<crd::usize>(i)] = static_cast<float>(x64[static_cast<crd::usize>(i)]); }

    const kir::KOp ops[2] = {kir::KOp::Add, kir::KOp::Max};
    for (int oi = 0; oi < 2; ++oi)
    {
        kir::KGraph          g0(&alloc);
        kir::KGraph          g1(&alloc);
        kir::KGraph*         graphs[2] = {&g0, &g1};
        const kir::ReducePlan plan     = kir::build_reduce(graphs, n, ops[oi], 256, 64);
        REQUIRE_FALSE(plan.single_pass);

        crd::containers::Array<crd::f64> part64(&alloc); part64.resize(static_cast<crd::usize>(plan.nblocks), 0.0);
        crd::f64                         out64 = -1234.0;
        kir::KernelBuffer kb0[2] = {{x64.data(), n, 0, 0}, {part64.data(), plan.nblocks, 0, 1}};
        kir::eval_cpu_kernel(*plan.block_graph, plan.block, kb0, 2, plan.block.local_size[0], &alloc, static_cast<crd::u32>(plan.nblocks));
        kir::KernelBuffer kb1[2] = {{part64.data(), plan.nblocks, 0, 0}, {&out64, 1, 0, 1}};
        kir::eval_cpu_kernel(*plan.final_graph, plan.final_pass, kb1, 2, plan.final_pass.local_size[0], &alloc, 1U);

        kir::GlslKernel kb(&alloc); kir::GlslKernel kf(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.block_graph, plan.block, &alloc, kb));
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.final_graph, plan.final_pass, &alloc, kf));
        auto pb = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kb.source), 2, 0U);
        auto pf = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kf.source), 2, 0U);
        REQUIRE(pb != nullptr); REQUIRE(pf != nullptr);

        crd::containers::Array<float> part32(&alloc); part32.resize(static_cast<crd::usize>(plan.nblocks), 0.0F);
        float                         out32 = -1234.0F;
        float*    h0[2]  = {x32.data(), part32.data()};
        int       l0[2]  = {n, plan.nblocks};
        crd::kir_test::dispatch_kernel_1wg(ctx, *pb, h0, l0, 2, static_cast<crd::u32>(plan.nblocks));
        float*    h1[2]  = {part32.data(), &out32};
        int       l1[2]  = {plan.nblocks, 1};
        crd::kir_test::dispatch_kernel_1wg(ctx, *pf, h1, l1, 2, 1U);

        CHECK(out32 == static_cast<float>(out64));
    }
}

// B-cmp: the CKIR device-wide SCAN dispatches bit-exact on DX12 (HLSL) vs the CPU oracle — the 3-pass plan lowers to HLSL
// identically (blocked shared scan, Hillis-Steele cross-thread via Select+Materialize). Inclusive + exclusive.
TEST_CASE("B-cmp: CKIR device SCAN DISPATCHES on DX12 == CPU oracle bit-exact", "[dx12][compute][gpu][kernel][scan]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    constexpr int n = 65536;
    crd::containers::Array<crd::f64> x64(&alloc); crd::containers::Array<float> x32(&alloc);
    x64.resize(n); x32.resize(n);
    for (int i = 0; i < n; ++i) { x64[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 7 + 3) % 5); x32[static_cast<crd::usize>(i)] = static_cast<float>(x64[static_cast<crd::usize>(i)]); }

    for (int incl = 0; incl < 2; ++incl)
    {
        kir::KGraph g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc); kir::KGraph* gs[3] = {&g0, &g1, &g2};
        const kir::ScanPlan plan = kir::build_scan(gs, n, incl != 0, 256, 64);
        REQUIRE_FALSE(plan.single_pass);
        const int nb = plan.nblocks;

        crd::containers::Array<crd::f64> loc64(&alloc); loc64.resize(n, 0.0);
        crd::containers::Array<crd::f64> bs64(&alloc);  bs64.resize(static_cast<crd::usize>(nb), 0.0);
        crd::containers::Array<crd::f64> of64(&alloc);  of64.resize(static_cast<crd::usize>(nb), 0.0);
        crd::containers::Array<crd::f64> out64(&alloc); out64.resize(n, 0.0);
        crd::f64 dummy = 0.0;
        kir::KernelBuffer a0[3] = {{x64.data(), n, 0, 0}, {loc64.data(), n, 0, 1}, {bs64.data(), nb, 0, 2}};
        kir::eval_cpu_kernel(*plan.block_graph, plan.block, a0, 3, plan.block.local_size[0], &alloc, static_cast<crd::u32>(nb));
        kir::KernelBuffer a1[3] = {{bs64.data(), nb, 0, 0}, {of64.data(), nb, 0, 1}, {&dummy, 1, 0, 2}};
        kir::eval_cpu_kernel(*plan.sums_graph, plan.scan_sums, a1, 3, plan.scan_sums.local_size[0], &alloc, 1U);
        kir::KernelBuffer a2[3] = {{loc64.data(), n, 0, 0}, {of64.data(), nb, 0, 1}, {out64.data(), n, 0, 2}};
        kir::eval_cpu_kernel(*plan.addoff_graph, plan.add_off, a2, 3, plan.add_off.local_size[0], &alloc, static_cast<crd::u32>(nb));

        kir::GlslKernel kk0(&alloc); kir::GlslKernel kk1(&alloc); kir::GlslKernel kk2(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.block_graph, plan.block, &alloc, kk0));
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.sums_graph, plan.scan_sums, &alloc, kk1));
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.addoff_graph, plan.add_off, &alloc, kk2));
        auto p0 = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kk0.source), 3, 0U);
        auto p1 = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kk1.source), 3, 0U);
        auto p2 = ctx.create_pipeline_from_hlsl(crd::containers::to_view(kk2.source), 3, 0U);
        REQUIRE(p0 != nullptr); REQUIRE(p1 != nullptr); REQUIRE(p2 != nullptr);

        crd::containers::Array<float> loc32(&alloc); loc32.resize(n, 0.0F);
        crd::containers::Array<float> bs32(&alloc);  bs32.resize(static_cast<crd::usize>(nb), 0.0F);
        crd::containers::Array<float> of32(&alloc);  of32.resize(static_cast<crd::usize>(nb), 0.0F);
        crd::containers::Array<float> out32(&alloc); out32.resize(n, 0.0F);
        float dm = 0.0F;
        float* hb0[3] = {x32.data(), loc32.data(), bs32.data()}; int lb0[3] = {n, n, nb};
        crd::kir_test::dispatch_kernel_1wg(ctx, *p0, hb0, lb0, 3, static_cast<crd::u32>(nb));
        float* hb1[3] = {bs32.data(), of32.data(), &dm}; int lb1[3] = {nb, nb, 1};
        crd::kir_test::dispatch_kernel_1wg(ctx, *p1, hb1, lb1, 3, 1U);
        float* hb2[3] = {loc32.data(), of32.data(), out32.data()}; int lb2[3] = {n, nb, n};
        crd::kir_test::dispatch_kernel_1wg(ctx, *p2, hb2, lb2, 3, static_cast<crd::u32>(nb));

        int bad = 0;
        for (int i = 0; i < n; ++i) { if (out32[static_cast<crd::usize>(i)] != static_cast<float>(out64[static_cast<crd::usize>(i)])) { ++bad; } }
        CHECK(bad == 0);
    }
}
