// test_dx12_compute.cpp — the D3D12 IComputeContext (ADR-0100) on the GPU. Proves the ONE backend-agnostic dispatch
// surface is genuinely backend-neutral: the SAME crd::gpu::IComputeContext API that drives geometry + CKIR on Vulkan
// runs a real kernel on DirectX 12. Vector-add through the full portable path (upload → dispatch → copy-readback),
// verified vs the CPU. This is the second backend that the seam claims to support — here it is, running.

#include <crd/gpu/dx12_compute_context.hpp>

#include <crd/kir/ckir.hpp>        // B-cmp: KGraph/KEntry for the shared-memory compute kernel
#include <crd/kir/ckir_fft.hpp>    // B-cmp Phase 1: build_fft1d_radix2 (the CKIR FFT authoring layer)
#include <crd/kir/ckir_reduce.hpp> // B-cmp: build_reduce (the CKIR device-wide reduction)
#include <crd/kir/ckir_scan.hpp>   // B-cmp: build_scan (the CKIR device-wide prefix sum)
#include <crd/kir/ckir_hlsl.hpp> // B-cmp: emit_compute_kernel_hlsl (the DX12 kernel emitter)

#include <crd/math/cmath.hpp>        // Phase-1 FFT: host-side twiddle table
#include <ckir_kernel_dispatch.hpp> // B-cmp: the SHARED both-backend kernel dispatch + oracle-compare harness

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
