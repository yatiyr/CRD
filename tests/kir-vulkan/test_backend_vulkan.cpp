// test_backend_vulkan.cpp — Phase 3.1.6 v17-b: the FIRST CKIR kernel on the GPU. Runs a fused-elementwise kernel
// through KirBackendVulkan (RTX 4070 Ti) and gates it against the CPU-reference oracle: arithmetic is BIT-EXACT
// (IEEE f32, `precise`/NoContraction), transcendentals are ULP-tolerant (until v17-h ports crd::math to GLSL); the
// kernel replays BIT-IDENTICAL ×3 (T1 determinism); ValidationCapture reports 0 errors. ADR-0098 DoD §6.

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_mlp.hpp>
#include <crd/kir/vulkan/backend_vulkan.hpp>

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm> // std::sort on a C array — the sorted-values oracle for the radix test
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace kir = crd::kir;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kN = 1024;

void fill(float* v, int n, float base) { for (int i = 0; i < n; ++i) { v[i] = base + 0.013F * static_cast<float>(i) - 0.5F * static_cast<float>(i % 7); } }
} // namespace

TEST_CASE("v17-b: first CKIR kernel on the GPU -- arithmetic bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping GPU test"); return; }
    kir::KirBackendCpu cpu(&alloc);

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({kN});
    const int        x  = g.input(sh, kir::DType::F32);
    const int        y  = g.input(sh, kir::DType::F32);
    // out = (x + y) * x - y * y + x * x  — Add/Sub/Mul are IEEE-correctly-rounded (+ `precise` ⇒ no FMA fusion) ⇒
    // BIT-EXACT vs the CPU reference. (GPU f32 division is a fast ~2-ULP reciprocal, NOT correctly-rounded ⇒ it is
    // ULP-tolerant here and gets correctly-rounded in v17-f via the float_controls audit — see the gotchas doc.)
    const int out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, y), x), g.binary(kir::KOp::Mul, y, y)), g.binary(kir::KOp::Mul, x, x));

    float xv[kN];
    float yv[kN];
    fill(xv, kN, 1.0F);
    fill(yv, kN, -0.7F);
    const float* inputs[] = {xv, yv};
    float        gpu_out[kN];
    float        cpu_out[kN];
    REQUIRE(vk.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));

    for (int i = 0; i < kN; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // BIT-EXACT (arith, precise)
    CHECK(vk.validation_errors() == 0);                              // DoD §6: clean validation

    // T1 determinism: run ×3, bit-identical
    float d2[kN];
    float d3[kN];
    REQUIRE(vk.run(g, out, inputs, 2, d2));
    REQUIRE(vk.run(g, out, inputs, 2, d3));
    for (int i = 0; i < kN; ++i) { CHECK(gpu_out[i] == d2[i]); CHECK(d2[i] == d3[i]); }
}

TEST_CASE("v17-b: GPU transcendental kernel matches the CPU oracle within ULP", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping GPU test"); return; }
    kir::KirBackendCpu cpu(&alloc);

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({kN});
    const int        x  = g.input(sh, kir::DType::F32);
    // out = tanh(x) * x + sqrt(|x| + 1)  — tanh differs GLSL-vs-crd::math until v17-h ⇒ ULP-tolerant
    const int one = g.constant(1.0, sh, kir::DType::F32);
    const int out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.unary(kir::KOp::Tanh, x), x),
                             g.unary(kir::KOp::Sqrt, g.binary(kir::KOp::Add, g.unary(kir::KOp::Abs, x), one)));

    float xv[kN];
    fill(xv, kN, 0.3F);
    const float* inputs[] = {xv};
    float        gpu_out[kN];
    float        cpu_out[kN];
    REQUIRE(vk.run(g, out, inputs, 1, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 1, cpu_out));
    for (int i = 0; i < kN; ++i) { CHECK_THAT(static_cast<double>(gpu_out[i]), WithinAbs(static_cast<double>(cpu_out[i]), 1e-5)); }
    CHECK(vk.validation_errors() == 0);
}

TEST_CASE("v17-b: GPU matmul (Contract) bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping GPU test"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int mm = 32;
    constexpr int kk = 48;
    constexpr int nn = 24;
    kir::KGraph   g(&alloc);
    const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     c = g.contract(a, b); // [mm, nn]

    float av[mm * kk];
    float bv[kk * nn];
    fill(av, mm * kk, 0.2F);
    fill(bv, kk * nn, -0.15F);
    const float* inputs[] = {av, bv};
    float        gpu_out[mm * nn];
    float        cpu_out[mm * nn];
    REQUIRE(vk.run(g, c, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, c, inputs, 2, cpu_out));
    // sequential-k, `precise` product+accumulation, dtype-faithful CPU reference ⇒ BIT-EXACT GPU matmul
    for (int i = 0; i < mm * nn; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
    CHECK(vk.validation_errors() == 0);
}

TEST_CASE("v17-h: Vulkan T2 FAST tiled GEMM (FMA, transposed-A) matches the oracle within tolerance + is deterministic", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping GPU test"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int mm = 128; // 64x64x8-tileable single-batch => routes to emit_contract_fast_glsl (the ported crush kernel)
    constexpr int kk = 64;
    constexpr int nn = 128;
    kir::KGraph   g(&alloc);
    const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     c = g.contract(a, b, kir::DetTier::Fast);

    float av[mm * kk];
    float bv[kk * nn];
    fill(av, mm * kk, 0.2F);
    fill(bv, kk * nn, -0.15F);
    const float* inputs[] = {av, bv};
    float        gpu_out[mm * nn];
    float        cpu_out[mm * nn];
    REQUIRE(vk.run(g, c, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, c, inputs, 2, cpu_out));
    // Fast tier = FMA + tiled reorder ⇒ NOT bit-exact vs the sequential `precise` oracle; relative-tolerance check.
    float maxrel = 0.0F;
    for (int i = 0; i < mm * nn; ++i) { const float rd = std::fabs(gpu_out[i] - cpu_out[i]) / (std::fabs(cpu_out[i]) + 1e-3F); if (rd > maxrel) { maxrel = rd; } }
    CHECK(maxrel < 1e-4F);
    // T2 determinism: fixed tile order + no atomics ⇒ run-to-run BIT-IDENTICAL.
    float d2[mm * nn];
    REQUIRE(vk.run(g, c, inputs, 2, d2));
    for (int i = 0; i < mm * nn; ++i) { CHECK(gpu_out[i] == d2[i]); }
    CHECK(vk.validation_errors() == 0);
}

namespace
{
// Tensor GEMM benchmark on the ONE unified compute context (ADR-0100): fp16-convert A/B, device-local + staging, timed
// dispatch loop, fp32 readback. Uses VulkanComputeContext primitives directly (the deleted VulkanComputeDevice::gemm_tensor
// folded into this test harness). Bindings 0=A(fp16) 1=B(fp16) 2=C(fp32); dims baked into the SPIR-V (no push).
[[nodiscard]] bool gemm_tensor_bench(crd::gpu::VulkanComputeContext& ctx, crd::containers::ConstSpan<crd::u8> spirv,
                                     const float* a, const float* b, crd::u32 m, crd::u32 n, crd::u32 k, float* c,
                                     int reps, double& ms)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    auto pipe = ctx.create_pipeline_from_spirv(spirv, 3, 0U);
    if (pipe == nullptr) { return false; }

    const crd::u64 abytes = static_cast<crd::u64>(m) * k * 2U; // fp16
    const crd::u64 bbytes = static_cast<crd::u64>(k) * n * 2U;
    const crd::u64 cbytes = static_cast<crd::u64>(m) * n * sizeof(float);
    auto           buf_a   = ctx.create_buffer(abytes, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto           buf_b   = ctx.create_buffer(bbytes, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto           buf_c0  = ctx.create_buffer(cbytes, storage | transfer_src, g::ComputeMemory::GpuOnly);
    auto           buf_c1  = ctx.create_buffer(cbytes, storage | transfer_src, g::ComputeMemory::GpuOnly); // ping-pong out
    auto           stg_a   = ctx.create_buffer(abytes, transfer_src, g::ComputeMemory::CpuToGpu);
    auto           stg_b   = ctx.create_buffer(bbytes, transfer_src, g::ComputeMemory::CpuToGpu);
    auto           stg_c   = ctx.create_buffer(cbytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    if (buf_a == nullptr || buf_b == nullptr || buf_c0 == nullptr || buf_c1 == nullptr || stg_a == nullptr || stg_b == nullptr || stg_c == nullptr) { return false; }

    auto f2h = [](float f) -> crd::u16 { // fp32 -> IEEE half (round-toward-zero; benchmark-grade)
        crd::u32 x = 0;
        std::memcpy(&x, &f, 4);
        const crd::u32 sign = (x >> 16) & 0x8000U;
        const crd::i32 exp  = static_cast<crd::i32>((x >> 23) & 0xFFU) - 112;
        const crd::u32 man  = x & 0x7FFFFFU;
        if (exp <= 0) { return static_cast<crd::u16>(sign); }
        if (exp >= 31) { return static_cast<crd::u16>(sign | 0x7C00U); }
        return static_cast<crd::u16>(sign | (static_cast<crd::u32>(exp) << 10) | (man >> 13));
    };
    { auto* d = static_cast<crd::u16*>(stg_a->map()); for (crd::u64 e = 0, ne = static_cast<crd::u64>(m) * k; e < ne; ++e) { d[e] = f2h(a[e]); } stg_a->unmap(); }
    { auto* d = static_cast<crd::u16*>(stg_b->map()); for (crd::u64 e = 0, ne = static_cast<crd::u64>(k) * n; e < ne; ++e) { d[e] = f2h(b[e]); } stg_b->unmap(); }

    { auto& rec = ctx.begin(); rec.copy(*stg_a, *buf_a, 0U, 0U, abytes); rec.copy(*stg_b, *buf_b, 0U, 0U, bbytes); ctx.submit_and_wait(); }

    const crd::u32    gx      = n / 128U; // 2D grid: x = column blocks, y = row blocks
    const crd::u32    gy      = m / 128U;
    // Ping-pong outputs: consecutive dispatches write DIFFERENT C buffers ⇒ no write-after-write hazard ⇒ NO barrier
    // between them ⇒ they run back-to-back. The coopmat2 GEMM already saturates the GPU, so this measures the TRUE
    // sustained tensor throughput, not the serialized rate polluted by ~0.05 ms per-barrier GPU stalls (nsys busy-rate =
    // ~68 TF; a barrier-between-every-dispatch loop under-reports it to ~57).
    g::ComputeBuffer* binds0[] = {buf_a.get(), buf_b.get(), buf_c0.get()};
    g::ComputeBuffer* binds1[] = {buf_a.get(), buf_b.get(), buf_c1.get()};
    const auto        span0    = crd::containers::ConstSpan<g::ComputeBuffer*>(binds0, 3);
    const auto        span1    = crd::containers::ConstSpan<g::ComputeBuffer*>(binds1, 3);

    { auto& rec = ctx.begin(); rec.dispatch(*pipe, span0, nullptr, 0U, gx, gy, 1U); ctx.submit_and_wait(); } // warm (absorb JIT)

    {
        auto& rec = ctx.begin();
        for (int i = 0; i < reps; ++i) { rec.dispatch(*pipe, (i & 1) != 0 ? span1 : span0, nullptr, 0U, gx, gy, 1U); }
        ctx.submit_and_wait();
        ms = ctx.last_gpu_ms() / static_cast<double>(reps); // GPU-only device timestamp (excludes CPU record/submit)
    }

    { auto& rec = ctx.begin(); rec.barrier(*buf_c0, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc); rec.copy(*buf_c0, *stg_c, 0U, 0U, cbytes); ctx.submit_and_wait(); }
    { const auto* rd = static_cast<const float*>(stg_c->map()); for (crd::u64 e = 0, on = static_cast<crd::u64>(m) * n; e < on; ++e) { c[e] = rd[e]; } stg_c->unmap(); }
    return true;
}
} // namespace

TEST_CASE("v17-i TENSOR: Vulkan coopmat2 GEMM through the UNIFIED compute context -- CUDA-parity + correct within fp16 tolerance", "[kir][vulkan][gpu][.bench]")
{
    // v17-i/ADR-0100: the tensor tier runs through the ONE unified VulkanComputeContext (the same surface geometry uses).
    // This test IS the perf harness (timing lives here, not on the production KirBackendVulkan).
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig cfg{};
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vkctx->cooperative_matrix2()) { WARN("no VK_NV_cooperative_matrix2 on this adapter; skipping tensor tier"); return; }

    crd::memory::TlsfAllocator alloc(256 << 20);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    constexpr int nn  = 2048; // 128-tileable
    const int     cnt = nn * nn;
    float*        av  = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(cnt) * sizeof(float)));
    float*        bv  = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(cnt) * sizeof(float)));
    float*        ov  = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(cnt) * sizeof(float)));
    for (int i = 0; i < cnt; ++i) // fp16-safe bounded inputs (|dot| stays well under fp16 range)
    {
        av[i] = 0.01F * static_cast<float>((i * 7) % 13 - 6);
        bv[i] = 0.01F * static_cast<float>((i * 5) % 11 - 5);
    }

    kir::GlslKernel kern(&alloc); // emit the coopmat2 kernel with K,N baked as literals ⇒ the driver specializes
    REQUIRE(kir::emit_contract_coopmat2_glsl(kern, static_cast<crd::u32>(nn), static_cast<crd::u32>(nn)));
    const auto cres = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_tensor", &alloc);
    if (!cres.ok)
    {
        WARN("shaderc build lacks coopmat2 support; skipping");
        alloc.deallocate(av);
        alloc.deallocate(bv);
        alloc.deallocate(ov);
        return;
    }

    double ms = 0.0;
    REQUIRE(gemm_tensor_bench(compute, crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), av, bv,
                              static_cast<crd::u32>(nn), static_cast<crd::u32>(nn), static_cast<crd::u32>(nn), ov, 500, ms));

    float maxrel = 0.0F; // a few outputs vs an fp32 host dot-product (fp16-input accuracy)
    for (int rr = 0; rr < 4; ++rr)
    {
        for (int cc = 0; cc < 4; ++cc)
        {
            double t = 0.0;
            for (int k = 0; k < nn; ++k) { t += static_cast<double>(av[rr * nn + k]) * static_cast<double>(bv[k * nn + cc]); }
            const double rel = std::fabs(static_cast<double>(ov[rr * nn + cc]) - t) / (std::fabs(t) + 1e-3);
            if (rel > static_cast<double>(maxrel)) { maxrel = static_cast<float>(rel); }
        }
    }
    std::printf("[Vulkan TENSOR coopmat2 N=%d via compute-device] %.4f ms = %.0f GFLOP/s  maxrel(vs fp32)=%.2e\n",
                nn, ms, 2.0 * nn * nn * nn / (ms * 1e6), static_cast<double>(maxrel));
    CHECK(maxrel < 5e-2F); // fp16-input tolerance
    alloc.deallocate(av);
    alloc.deallocate(bv);
    alloc.deallocate(ov);
}

namespace
{
crd::u16 f2h_mlp(float f) // fp32 -> IEEE half (round-toward-zero; test-grade)
{
    crd::u32 x = 0;
    std::memcpy(&x, &f, 4);
    const crd::u32 sign = (x >> 16) & 0x8000U;
    const crd::i32 exp  = static_cast<crd::i32>((x >> 23) & 0xFFU) - 112;
    const crd::u32 man  = x & 0x7FFFFFU;
    if (exp <= 0) { return static_cast<crd::u16>(sign); }
    if (exp >= 31) { return static_cast<crd::u16>(sign | 0x7C00U); }
    return static_cast<crd::u16>(sign | (static_cast<crd::u32>(exp) << 10) | (man >> 13));
}
float h2f_mlp(crd::u16 h) // IEEE half -> fp32
{
    const crd::u32 sign = (static_cast<crd::u32>(h) & 0x8000U) << 16;
    const crd::u32 exp  = (static_cast<crd::u32>(h) >> 10) & 0x1FU;
    const crd::u32 man  = static_cast<crd::u32>(h) & 0x3FFU;
    crd::u32       out  = 0;
    if (exp == 0) { out = sign; } // treat subnormals as 0 (matches the f2h flush)
    else if (exp == 31) { out = sign | 0x7F800000U | (man << 13); }
    else { out = sign | ((exp + 112U) << 23) | (man << 13); }
    float f = 0.0F;
    std::memcpy(&f, &out, 4);
    return f;
}
} // namespace

// CKIR-AUTHORED fused MLP FORWARD on Vulkan via coopmat2 — the SAME MlpConfig the CUDA port used, now on the second
// primary compute backend. Emits the coopmat2 GLSL, runs it through the unified compute context, gates the fp16 result
// against the CPU reference oracle (fp16 tolerance) + a run-to-run determinism replay. This is what makes the crush portable.
TEST_CASE("v17 MLP: CKIR fused-MLP forward on Vulkan coopmat2 == CPU oracle (fp16 tol) + deterministic", "[kir][vulkan][gpu][mlp]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vkctx->cooperative_matrix2()) { WARN("no VK_NV_cooperative_matrix2 on this adapter; skipping MLP tensor tier"); return; }

    crd::memory::TlsfAllocator alloc(128 << 20);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    kir::MlpConfig cfg;
    cfg.width      = 64;
    cfg.layers     = 6;
    cfg.batch_tile = 64; // GLSL tile: 6·64·64 = 24 KB shared (safe under 48 KB)
    cfg.warps      = 2;
    REQUIRE(cfg.valid());
    const int wd    = cfg.width;
    const int nl    = cfg.layers;
    const int batch = 8192;
    const int n_in  = batch * wd;
    const int n_w   = nl * wd * wd;

    // fp16-quantized inputs (so the oracle sees exactly what the GPU sees; isolates the fp16-accumulation difference).
    // Bounded small so the 6-layer fp16 chain stays close to the fp32 reference.
    float* in_f = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(n_in) * sizeof(float)));
    float* w_f  = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(n_w) * sizeof(float)));
    for (int i = 0; i < n_in; ++i) { in_f[i] = h2f_mlp(f2h_mlp(0.20F * static_cast<float>((i * 7) % 13 - 6))); }
    for (int i = 0; i < n_w; ++i) { w_f[i] = h2f_mlp(f2h_mlp(0.10F * static_cast<float>((i * 5) % 11 - 5))); }

    // CPU oracle (fp32 reference)
    float* out_ref = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(n_in) * sizeof(float)));
    float* sa      = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(wd) * sizeof(float)));
    float* sb      = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(wd) * sizeof(float)));
    kir::mlp_forward_ref(cfg, in_f, w_f, sa, sb, out_ref, batch);

    // emit + compile the coopmat2 GLSL
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_fused_mlp_fwd_glsl(cfg, kern));
    const auto cres = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_mlp", &alloc);
    if (!cres.ok)
    {
        WARN("shaderc build lacks coopmat2 support; skipping");
        return;
    }

    using gpu::compute_usage::storage;
    using gpu::compute_usage::transfer_dst;
    using gpu::compute_usage::transfer_src;
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    const crd::u64 in_bytes  = static_cast<crd::u64>(n_in) * 2U; // fp16
    const crd::u64 w_bytes   = static_cast<crd::u64>(n_w) * 2U;
    const crd::u64 out_bytes = static_cast<crd::u64>(n_in) * 2U;
    auto           buf_in    = compute.create_buffer(in_bytes, storage | transfer_dst, gpu::ComputeMemory::GpuOnly);
    auto           buf_w     = compute.create_buffer(w_bytes, storage | transfer_dst, gpu::ComputeMemory::GpuOnly);
    auto           buf_out   = compute.create_buffer(out_bytes, storage | transfer_src, gpu::ComputeMemory::GpuOnly);
    auto           stg_in    = compute.create_buffer(in_bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
    auto           stg_w     = compute.create_buffer(w_bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
    auto           stg_out   = compute.create_buffer(out_bytes, transfer_dst, gpu::ComputeMemory::GpuToCpu);
    REQUIRE(buf_in != nullptr);
    REQUIRE(buf_w != nullptr);
    REQUIRE(buf_out != nullptr);
    REQUIRE(stg_in != nullptr);
    REQUIRE(stg_w != nullptr);
    REQUIRE(stg_out != nullptr);

    { auto* d = static_cast<crd::u16*>(stg_in->map()); for (int i = 0; i < n_in; ++i) { d[i] = f2h_mlp(in_f[i]); } stg_in->unmap(); }
    { auto* d = static_cast<crd::u16*>(stg_w->map()); for (int i = 0; i < n_w; ++i) { d[i] = f2h_mlp(w_f[i]); } stg_w->unmap(); }
    { auto& rec = compute.begin(); rec.copy(*stg_in, *buf_in, 0U, 0U, in_bytes); rec.copy(*stg_w, *buf_w, 0U, 0U, w_bytes); compute.submit_and_wait(); }

    gpu::ComputeBuffer* binds[] = {buf_in.get(), buf_w.get(), buf_out.get()};
    const auto          span    = crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 3);
    const crd::u32      groups  = static_cast<crd::u32>(batch / cfg.batch_tile);

    const auto readback = [&](float* dst) {
        { auto& rec = compute.begin(); rec.dispatch(*pipe, span, nullptr, 0U, groups, 1U, 1U); compute.submit_and_wait(); }
        { auto& rec = compute.begin(); rec.barrier(*buf_out, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::TransferSrc); rec.copy(*buf_out, *stg_out, 0U, 0U, out_bytes); compute.submit_and_wait(); }
        const auto* rd = static_cast<const crd::u16*>(stg_out->map());
        for (int i = 0; i < n_in; ++i) { dst[i] = h2f_mlp(rd[i]); }
        stg_out->unmap();
    };

    float* got = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(n_in) * sizeof(float)));
    float* got2 = static_cast<float*>(alloc.allocate(static_cast<crd::usize>(n_in) * sizeof(float)));
    readback(got);
    readback(got2);

    // correctness vs the oracle (fp16 tolerance, normalized by output magnitude)
    double maxabs = 0.0;
    double maxmag = 0.0;
    for (int i = 0; i < n_in; ++i)
    {
        const double d = std::fabs(static_cast<double>(got[i]) - static_cast<double>(out_ref[i]));
        if (d > maxabs) { maxabs = d; }
        const double m = std::fabs(static_cast<double>(out_ref[i]));
        if (m > maxmag) { maxmag = m; }
    }
    // run-to-run determinism: the fixed coopmat schedule (no atomics) must replay bit-identical
    int det_diff = 0;
    for (int i = 0; i < n_in; ++i) { if (got[i] != got2[i]) { ++det_diff; } }
    std::printf("[Vulkan MLP coopmat2] batch=%d W=%d L=%d  max_abs=%.4f max_mag=%.4f rel=%.4f  det_diff=%d/%d\n",
                batch, wd, nl, maxabs, maxmag, maxabs / (maxmag + 1e-6), det_diff, n_in);
    CHECK(maxmag > 1e-3);                       // the network actually produced signal
    CHECK(maxabs / (maxmag + 1e-6) < 5e-2);     // fp16 tolerance vs fp32 oracle
    CHECK(det_diff == 0);                       // bit-identical run-to-run (determinism pillar)

    alloc.deallocate(in_f);
    alloc.deallocate(w_f);
    alloc.deallocate(out_ref);
    alloc.deallocate(sa);
    alloc.deallocate(sb);
    alloc.deallocate(got);
    alloc.deallocate(got2);
}

TEST_CASE("v17-g: Vulkan FUSES GEMM+bias+SiLU into one kernel, correct vs the oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping GPU test"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int mm = 128;
    constexpr int kk = 96;
    constexpr int nn = 128;
    kir::KGraph   g(&alloc);
    const int     a    = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b    = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     bias = g.input(kir::make_shape({nn}), kir::DType::F32);
    const int     c    = g.contract(a, b);
    const int     bc   = g.broadcast(bias, kir::make_shape({mm, nn}));
    const int     z    = g.binary(kir::KOp::Add, c, bc);
    const int     nz   = g.unary(kir::KOp::Neg, z);
    const int     ez   = g.unary(kir::KOp::Exp, nz);
    const int     one  = g.constant(1.0, kir::make_shape({mm, nn}), kir::DType::F32);
    const int     den  = g.binary(kir::KOp::Add, one, ez);
    const int     sig  = g.unary(kir::KOp::Recip, den);
    const int     out  = g.binary(kir::KOp::Mul, z, sig); // SiLU(A@B + bias)

    crd::containers::Array<float> av(&alloc);
    crd::containers::Array<float> bv(&alloc);
    crd::containers::Array<float> biasv(&alloc);
    crd::containers::Array<float> gpu(&alloc);
    crd::containers::Array<float> cpuo(&alloc);
    av.resize(static_cast<crd::usize>(mm) * kk);
    bv.resize(static_cast<crd::usize>(kk) * nn);
    biasv.resize(static_cast<crd::usize>(nn));
    gpu.resize(static_cast<crd::usize>(mm) * nn);
    cpuo.resize(static_cast<crd::usize>(mm) * nn);
    fill(av.data(), mm * kk, 0.1F);
    fill(bv.data(), kk * nn, -0.05F);
    fill(biasv.data(), nn, 0.2F);
    const float* inputs[] = {av.data(), bv.data(), biasv.data()};
    REQUIRE(vk.run(g, out, inputs, 3, gpu.data()));  // → the fused GLSL kernel (ONE dispatch)
    REQUIRE(cpu.run(g, out, inputs, 3, cpuo.data())); // reference
    float maxrel = 0.0F;
    for (int i = 0; i < mm * nn; ++i)
    {
        const float d  = (gpu[i] - cpuo[i]) / (1.0F + (cpuo[i] < 0.0F ? -cpuo[i] : cpuo[i]));
        const float ad = d < 0.0F ? -d : d;
        if (ad > maxrel) { maxrel = ad; }
    }
    CHECK(maxrel < 1e-3F);
    CHECK(vk.validation_errors() == 0);
}

TEST_CASE("v17-b: GPU fixed-order reduce (sum/max) bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping GPU test"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rows = 40;
    constexpr int cols = 96;
    float         xv[rows * cols];
    fill(xv, rows * cols, 0.25F);
    const float* inputs[] = {xv};

    SECTION("sum over the last axis [rows,cols] -> [rows,1]")
    {
        kir::KGraph g(&alloc);
        const int   a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int   red = g.reduce(kir::KOp::ReduceSum, a, 0x2U); // reduce axis 1 (trailing)
        float       gpu_out[rows];
        float       cpu_out[rows];
        REQUIRE(vk.run(g, red, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
        for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // fixed ascending order ⇒ BIT-EXACT
    }
    SECTION("reduce-all sum + max")
    {
        kir::KGraph g(&alloc);
        const int   a    = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int   rsum = g.reduce(kir::KOp::ReduceSum, a, 0x3U); // all axes
        const int   rmax = g.reduce(kir::KOp::ReduceMax, a, 0x3U);
        float       gs[1];
        float       cs[1];
        float       gm[1];
        float       cm[1];
        REQUIRE(vk.run(g, rsum, inputs, 1, gs));
        REQUIRE(cpu.run(g, rsum, inputs, 1, cs));
        REQUIRE(vk.run(g, rmax, inputs, 1, gm));
        REQUIRE(cpu.run(g, rmax, inputs, 1, cm));
        CHECK(gs[0] == cs[0]);
        CHECK(gm[0] == cm[0]);
    }
    SECTION("reduce-min + reduce-prod over rows (bit-exact vs oracle)")
    {
        const kir::KOp ops[2] = {kir::KOp::ReduceMin, kir::KOp::ReduceProd};
        for (int oi = 0; oi < 2; ++oi)
        {
            kir::KGraph g(&alloc);
            const int   a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
            const int   red = g.reduce(ops[oi], a, 0x2U);
            float       pv[rows * cols];
            for (int i = 0; i < rows * cols; ++i) { pv[i] = 0.98F + 0.0004F * static_cast<float>(i % 51); }
            const float* pin[] = {pv};
            float        gpu_out[rows];
            float        cpu_out[rows];
            REQUIRE(vk.run(g, red, pin, 1, gpu_out));
            REQUIRE(cpu.run(g, red, pin, 1, cpu_out));
            for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
        }
    }
    CHECK(vk.validation_errors() == 0);
}

TEST_CASE("v17-breadth: Vulkan floor/ceil/sign/cmpeq/cmple bit-match the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int    nn = 1024;
    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({nn});
    const int        x  = g.input(sh, kir::DType::F32);
    const int        y  = g.input(sh, kir::DType::F32);
    const int uf  = g.unary(kir::KOp::Floor, x);
    const int uc  = g.unary(kir::KOp::Ceil, x);
    const int us  = g.unary(kir::KOp::Sign, y);
    const int ut  = g.unary(kir::KOp::Trunc, y);
    // B0-3: comparisons are BOOL-typed, so folding them into float arithmetic needs an explicit cast. GLSL rejects
    // `float + bool` outright; HLSL silently promotes it — the IR-level cast is what keeps the two backends agreeing.
    const int bce = g.cast(g.binary(kir::KOp::CmpEq, x, y), kir::DType::F32);
    const int bcl = g.cast(g.binary(kir::KOp::CmpLe, x, y), kir::DType::F32);
    const int out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, uf, uc), g.binary(kir::KOp::Add, us, bce)), bcl), ut);

    float xv[nn];
    float yv[nn];
    for (int i = 0; i < nn; ++i)
    {
        xv[i] = -3.5F + 0.017F * static_cast<float>(i % 401);
        yv[i] = (i % 5 == 0) ? xv[i] : (xv[i] + 0.5F * static_cast<float>((i % 3) - 1));
    }
    const float* inputs[] = {xv, yv};
    float        gpu_out[nn];
    float        cpu_out[nn];
    REQUIRE(be.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
    for (int i = 0; i < nn; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // floor/ceil/sign/cmp all exact => BIT-EXACT
}

TEST_CASE("v17-breadth: Vulkan gather row index-select bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rr = 50; // data rows
    constexpr int cc = 8;  // row width
    constexpr int mm = 20; // gather count
    kir::KGraph   g(&alloc);
    const int     data = g.input(kir::make_shape({rr, cc}), kir::DType::F32);
    const int     idx  = g.input(kir::make_shape({mm}), kir::DType::F32);
    const int     out  = g.gather(data, idx);
    float dv[rr * cc];
    for (int i = 0; i < rr * cc; ++i) { dv[i] = 0.1F * static_cast<float>(i) - 3.0F; }
    float iv[mm];
    for (int i = 0; i < mm; ++i) { iv[i] = static_cast<float>((i * 7 + 3) % rr); }
    const float* inputs[] = {dv, iv};
    float        gpu_out[mm * cc];
    float        cpu_out[mm * cc];
    REQUIRE(be.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
    for (int i = 0; i < mm * cc; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // gather = pure copy => BIT-EXACT
}

TEST_CASE("v17-breadth: Vulkan argmax/argmin index bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int  rows   = 40;
    constexpr int  cols   = 96;
    const kir::KOp ops[2] = {kir::KOp::ArgMax, kir::KOp::ArgMin};
    for (int oi = 0; oi < 2; ++oi)
    {
        kir::KGraph g(&alloc);
        const int   a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int   red = g.reduce(ops[oi], a, 0x2U);
        float       xv[rows * cols];
        for (int i = 0; i < rows * cols; ++i) { xv[i] = static_cast<float>((i * 37) % 91) * 0.1F; } // varied; first-match ties
        const float* inputs[] = {xv};
        float        gpu_out[rows];
        float        cpu_out[rows];
        REQUIRE(be.run(g, red, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
        for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // the extremum INDEX, exact => BIT-EXACT
    }
}

TEST_CASE("v17-breadth: Vulkan round ties-to-even bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int    nn = 256;
    kir::KGraph      g(&alloc);
    const kir::Shape sh  = kir::make_shape({nn});
    const int        x   = g.input(sh, kir::DType::F32);
    const int        out = g.unary(kir::KOp::Round, x);
    float            xv[nn];
    for (int i = 0; i < nn; ++i) { xv[i] = -8.0F + 0.5F * static_cast<float>(i); } // every value is .0 or .5 => exercises ties-to-even
    const float* inputs[] = {xv};
    float        gpu_out[nn];
    float        cpu_out[nn];
    REQUIRE(be.run(g, out, inputs, 1, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 1, cpu_out));
    for (int i = 0; i < nn; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // ties-even == nearbyint => BIT-EXACT
}

TEST_CASE("v17-breadth: Vulkan scatter last-wins bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rr = 30; // base rows
    constexpr int cc = 8;  // row width
    constexpr int mm = 20; // scatter count (idx has DUPLICATES -> exercises deterministic last-wins)
    kir::KGraph   g(&alloc);
    const int     base = g.input(kir::make_shape({rr, cc}), kir::DType::F32);
    const int     idx  = g.input(kir::make_shape({mm}), kir::DType::F32);
    const int     upd  = g.input(kir::make_shape({mm, cc}), kir::DType::F32);
    const int     out  = g.scatter(base, idx, upd);
    float bv[rr * cc];
    for (int i = 0; i < rr * cc; ++i) { bv[i] = -1.0F - 0.1F * static_cast<float>(i); }
    float iv[mm];
    for (int i = 0; i < mm; ++i) { iv[i] = static_cast<float>((i * 3) % rr); } // i=0 and i=10 both hit row 0
    float uv[mm * cc];
    for (int i = 0; i < mm * cc; ++i) { uv[i] = 5.0F + 0.25F * static_cast<float>(i); }
    const float* inputs[] = {bv, iv, uv};
    float        gpu_out[rr * cc];
    float        cpu_out[rr * cc];
    REQUIRE(be.run(g, out, inputs, 3, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 3, cpu_out));
    for (int i = 0; i < rr * cc; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // last-wins, fixed order => BIT-EXACT
}

TEST_CASE("v17-breadth: Vulkan scan prefix-sum bit-matches the CPU oracle", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rows = 32;
    constexpr int cols = 48;
    kir::KGraph   g(&alloc);
    const int     a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
    const int     out = g.scan(a); // inclusive prefix-sum along cols
    float         xv[rows * cols];
    for (int i = 0; i < rows * cols; ++i) { xv[i] = static_cast<float>((i % 7) + 1); } // small ints => exact cumulative sums everywhere
    const float* inputs[] = {xv};
    float        gpu_out[rows * cols];
    float        cpu_out[rows * cols];
    REQUIRE(be.run(g, out, inputs, 1, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 1, cpu_out));
    for (int i = 0; i < rows * cols; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // exact integer prefix sums => BIT-EXACT
}

TEST_CASE("v17-perf: Vulkan T2 fast parallel reduce (workgroup tree) -- correct + deterministic", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(128 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rows = 8;
    constexpr int nlen = 4096; // long reduce axis => the parallel tree wins over one-thread-per-output
    kir::KGraph   g(&alloc);
    const int     a   = g.input(kir::make_shape({rows, nlen}), kir::DType::F32);
    const int     r2  = g.reduce(kir::KOp::ReduceSum, a, 0x2U, kir::DetTier::Fast); // T2 parallel tree-reduce

    crd::containers::Array<float> xv(&alloc);
    xv.resize(static_cast<crd::usize>(rows) * nlen);
    for (int i = 0; i < rows * nlen; ++i) { xv[i] = static_cast<float>((i % 5) + 1); } // small ints => order-invariant exact sums
    const float* inputs[] = {xv.data()};
    float        g1[rows];
    float        g2[rows];
    float        g3[rows];
    float        co[rows];
    REQUIRE(vk.run(g, r2, inputs, 1, g1));
    REQUIRE(cpu.run(g, r2, inputs, 1, co)); // T1 fixed-order oracle == exact for integer inputs
    REQUIRE(vk.run(g, r2, inputs, 1, g2));
    REQUIRE(vk.run(g, r2, inputs, 1, g3));
    for (int i = 0; i < rows; ++i)
    {
        CHECK(g1[i] == co[i]); // T2 tree-reduce == exact sum (reassociation is exact for these ints)
        CHECK(g1[i] == g2[i]); // run-to-run deterministic (T2 determinism guarantee)
        CHECK(g2[i] == g3[i]);
    }
    CHECK(vk.validation_errors() == 0);
}

TEST_CASE("v17-perf: Vulkan T2 parallel reduce speedup over T1 on a long-axis reduction", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    constexpr int nlen = 1 << 21; // 2M-element single reduction: T1 = ONE thread does 2M adds; T2 = 256-thread tree
    kir::KGraph   g(&alloc);
    const int     a   = g.input(kir::make_shape({1, nlen}), kir::DType::F32);
    const int     rt1 = g.reduce(kir::KOp::ReduceSum, a, 0x2U, kir::DetTier::Exact); // T1 sequential
    const int     rt2 = g.reduce(kir::KOp::ReduceSum, a, 0x2U, kir::DetTier::Fast);  // T2 parallel tree

    crd::containers::Array<float> xv(&alloc);
    xv.resize(static_cast<crd::usize>(nlen));
    for (int i = 0; i < nlen; ++i) { xv[i] = 1.0F; }
    const float* inputs[] = {xv.data()};
    float        o[1];
    REQUIRE(vk.run(g, rt1, inputs, 1, o)); // warm up both pipelines
    REQUIRE(vk.run(g, rt2, inputs, 1, o));

    constexpr int iters = 20;
    const auto    s0 = std::chrono::high_resolution_clock::now();
    for (int k = 0; k < iters; ++k) { REQUIRE(vk.run(g, rt1, inputs, 1, o)); }
    const auto    s1 = std::chrono::high_resolution_clock::now();
    for (int k = 0; k < iters; ++k) { REQUIRE(vk.run(g, rt2, inputs, 1, o)); }
    const auto    s2 = std::chrono::high_resolution_clock::now();
    const double  ms1 = std::chrono::duration<double, std::milli>(s1 - s0).count() / iters;
    const double  ms2 = std::chrono::duration<double, std::milli>(s2 - s1).count() / iters;
    WARN("2M-element reduce  T1(sequential)=" << ms1 << "ms  T2(parallel-tree)=" << ms2 << "ms  speedup=" << (ms1 / ms2) << "x (incl. upload+readback)");
    CHECK(vk.validation_errors() == 0);
}

TEST_CASE("v17-perf: Vulkan T2 fast reduce sum/prod/max/min matches T1 oracle + deterministic", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int  rows   = 16;
    constexpr int  cols   = 32; // small so prod stays exact (<= 2^32) under reassociation
    const kir::KOp ops[4] = {kir::KOp::ReduceSum, kir::KOp::ReduceProd, kir::KOp::ReduceMax, kir::KOp::ReduceMin};
    for (int oi = 0; oi < 4; ++oi)
    {
        kir::KGraph g(&alloc);
        const int   a  = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int   r2 = g.reduce(ops[oi], a, 0x2U, kir::DetTier::Fast); // T2 parallel tree-reduce
        float       xv[rows * cols];
        for (int i = 0; i < rows * cols; ++i) { xv[i] = (i % 3 == 0) ? 2.0F : 1.0F; } // {1,2}: sum/prod/max/min all reassociation-exact
        const float* inputs[] = {xv};
        float        g1[rows];
        float        g2[rows];
        float        co[rows];
        REQUIRE(be.run(g, r2, inputs, 1, g1));
        REQUIRE(cpu.run(g, r2, inputs, 1, co)); // T1 fixed-order oracle == exact for these inputs
        REQUIRE(be.run(g, r2, inputs, 1, g2));
        for (int i = 0; i < rows; ++i) { CHECK(g1[i] == co[i]); CHECK(g1[i] == g2[i]); } // T2 correct + run-to-run deterministic
    }
}

TEST_CASE("v17-perf: Vulkan T2 fast parallel scan matches T1 oracle + deterministic", "[kir][vulkan][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendVulkan        be(&alloc);
    if (!be.valid()) { WARN("no Vulkan device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rows = 4;
    constexpr int nlen = 1000; // > 256 => exercises the chunked parallel scan
    kir::KGraph   g(&alloc);
    const int     a  = g.input(kir::make_shape({rows, nlen}), kir::DType::F32);
    const int     s2 = g.scan(a, kir::DetTier::Fast); // T2 parallel prefix-sum
    crd::containers::Array<float> xv(&alloc);
    xv.resize(static_cast<crd::usize>(rows) * nlen);
    for (int i = 0; i < rows * nlen; ++i) { xv[i] = static_cast<float>((i % 4) + 1); } // small ints => order-invariant exact prefix sums
    const float* inputs[] = {xv.data()};
    crd::containers::Array<float> g1(&alloc);
    crd::containers::Array<float> g2(&alloc);
    crd::containers::Array<float> co(&alloc);
    g1.resize(static_cast<crd::usize>(rows) * nlen);
    g2.resize(static_cast<crd::usize>(rows) * nlen);
    co.resize(static_cast<crd::usize>(rows) * nlen);
    REQUIRE(be.run(g, s2, inputs, 1, g1.data()));
    REQUIRE(cpu.run(g, s2, inputs, 1, co.data())); // T1 fixed-order oracle == exact for integer inputs
    REQUIRE(be.run(g, s2, inputs, 1, g2.data()));
    for (int i = 0; i < rows * nlen; ++i) { CHECK(g1[i] == co[i]); CHECK(g1[i] == g2[i]); } // T2 correct + run-to-run deterministic
}

// ── v17-i: MORTON authored in CKIR, running on the GPU ──────────────────────────────────────────────────────────────
namespace morton_ckir
{
constexpr int kN = 256;
inline crd::u32 expand_ref(crd::u32 v)
{
    v = (v | (v << 16)) & 0x030000FFU;
    v = (v | (v << 8)) & 0x0300F00FU;
    v = (v | (v << 4)) & 0x030C30C3U;
    v = (v | (v << 2)) & 0x09249249U;
    return v;
}
inline crd::u32 ref(crd::u32 x, crd::u32 y, crd::u32 z) { return (expand_ref(x) << 2) | (expand_ref(y) << 1) | expand_ref(z); }
inline int      expand(kir::KGraph& g, int v, const kir::Shape& sh)
{
    auto konst = [&](crd::i64 c) { return g.constant(static_cast<crd::f64>(c), sh, kir::DType::I32); };
    auto shl   = [&](int a, crd::i64 b) { return g.binary(kir::KOp::Shl, a, konst(b)); };
    auto bor   = [&](int a, int b) { return g.binary(kir::KOp::BitOr, a, b); };
    auto band  = [&](int a, crd::i64 m) { return g.binary(kir::KOp::BitAnd, a, konst(m)); };
    v = band(bor(v, shl(v, 16)), 0x030000FF);
    v = band(bor(v, shl(v, 8)), 0x0300F00F);
    v = band(bor(v, shl(v, 4)), 0x030C30C3);
    v = band(bor(v, shl(v, 2)), 0x09249249);
    return v;
}
inline int build(kir::KGraph& g, const kir::Shape& sh)
{
    const int x = g.input(sh, kir::DType::F32);
    const int y = g.input(sh, kir::DType::F32);
    const int z = g.input(sh, kir::DType::F32);
    auto      konstf = [&](crd::f64 c) { return g.constant(c, sh, kir::DType::F32); };
    auto      quant  = [&](int c) {
        int s = g.binary(kir::KOp::Mul, c, konstf(1024.0));
        s     = g.binary(kir::KOp::Max, s, konstf(0.0));
        s     = g.binary(kir::KOp::Min, s, konstf(1023.0));
        s     = g.unary(kir::KOp::Floor, s);
        return g.cast(s, kir::DType::I32);
    };
    const int ex     = expand(g, quant(x), sh);
    const int ey     = expand(g, quant(y), sh);
    const int ez     = expand(g, quant(z), sh);
    auto      konsti = [&](crd::i64 c) { return g.constant(static_cast<crd::f64>(c), sh, kir::DType::I32); };
    return g.binary(kir::KOp::BitOr, g.binary(kir::KOp::BitOr, g.binary(kir::KOp::Shl, ex, konsti(2)), g.binary(kir::KOp::Shl, ey, konsti(1))), ez);
}
} // namespace morton_ckir

TEST_CASE("v17-i: Morton authored in CKIR runs on Vulkan, bit-exact vs the reference", "[kir][vulkan][gpu][morton]")
{
    namespace m = morton_ckir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    kir::KGraph      g(&alloc);
    const kir::Shape sh     = kir::make_shape({m::kN});
    const int        morton = m::build(g, sh);

    crd::u32 qx[m::kN];
    crd::u32 qy[m::kN];
    crd::u32 qz[m::kN];
    float    xv[m::kN];
    float    yv[m::kN];
    float    zv[m::kN];
    for (int i = 0; i < m::kN; ++i)
    {
        qx[i] = static_cast<crd::u32>((i * 7) % 1024);
        qy[i] = static_cast<crd::u32>((i * 13) % 1024);
        qz[i] = static_cast<crd::u32>((i * 29) % 1024);
        xv[i] = (static_cast<float>(qx[i]) + 0.5F) / 1024.0F;
        yv[i] = (static_cast<float>(qy[i]) + 0.5F) / 1024.0F;
        zv[i] = (static_cast<float>(qz[i]) + 0.5F) / 1024.0F;
    }
    const float* inputs[] = {xv, yv, zv};
    float        gpu_out[m::kN];
    REQUIRE(vk.run(g, morton, inputs, 3, gpu_out));

    int mism = 0;
    for (int i = 0; i < m::kN; ++i)
    {
        crd::u32 code = 0;
        std::memcpy(&code, &gpu_out[i], 4); // the I32 morton bits come back reinterpreted in the f32 readback slot
        if (code != m::ref(qx[i], qy[i], qz[i])) { ++mism; }
    }
    CHECK(mism == 0);
}

// ── v17-i rung 2: ATOMIC scatter-add (radix histogram) on the GPU ───────────────────────────────────────────────────
namespace histo_ckir
{
constexpr int   kHistoN = 1024; // elements
constexpr int   kHistoBins = 256; // bins (one radix digit)
inline float    asf(crd::i32 v) { float f = 0.0F; std::memcpy(&f, &v, 4); return f; }   // I32 bits → f32 slot (raw upload)
inline crd::u32 asu(float f) { crd::u32 u = 0; std::memcpy(&u, &f, 4); return u; }       // f32 readback slot → u32 bits
} // namespace histo_ckir

TEST_CASE("v17-i: CKIR scatter-add histogram runs on Vulkan (integer atomics, deterministic)", "[kir][vulkan][gpu][atomics]")
{
    namespace h = histo_ckir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    kir::KGraph      g(&alloc);
    const kir::Shape shn  = kir::make_shape({h::kHistoN});
    const kir::Shape shm  = kir::make_shape({h::kHistoBins});
    const int        idx  = g.input(shn, kir::DType::I32);
    const int        upd  = g.input(shn, kir::DType::I32);
    const int        hist = g.scatter_add(idx, upd, shm);

    float    idxv[h::kHistoN];
    float    updv[h::kHistoN];
    crd::u32 ref[h::kHistoBins];
    for (int i = 0; i < h::kHistoBins; ++i) { ref[i] = 0; }
    for (int i = 0; i < h::kHistoN; ++i)
    {
        const crd::i32 d = static_cast<crd::i32>((i * 7 + 13) % h::kHistoBins);
        idxv[i]          = h::asf(d);
        updv[i]          = h::asf(1);
        ref[d]++;
    }
    const float* inputs[] = {idxv, updv};
    float        out[h::kHistoBins];
    REQUIRE(vk.run(g, hist, inputs, 2, out));

    int mism = 0;
    for (int i = 0; i < h::kHistoBins; ++i) { if (h::asu(out[i]) != ref[i]) { ++mism; } }
    CHECK(mism == 0);
}

// ── v17-e: the MULTI-KERNEL SCHEDULER — a 2-stage GPU pipeline with an on-GPU intermediate ──────────────────────────
TEST_CASE("v17-e: scheduler runs ScanSum(Mul(x,y)) as a 2-kernel GPU pipeline (Mul result never leaves the GPU)", "[kir][vulkan][gpu][scheduler]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    kir::KGraph      g(&alloc);
    constexpr int    sn = 1024;
    const kir::Shape sh = kir::make_shape({sn});
    const int        x  = g.input(sh, kir::DType::F32);
    const int        y  = g.input(sh, kir::DType::F32);
    const int        m  = g.binary(kir::KOp::Mul, x, y); // kernel 1 (elementwise) → on-GPU intermediate
    const int        s  = g.scan(m);                     // kernel 2 (scan) reads the intermediate → inclusive prefix

    float xv[sn];
    float yv[sn];
    float ref[sn];
    for (int i = 0; i < sn; ++i)
    {
        xv[i] = 0.5F + (0.001F * static_cast<float>(i));
        yv[i] = 1.0F - (0.0005F * static_cast<float>(i));
    }
    float facc = 0.0F; // reference: single-rounded f32 mul + fixed-order f32 accumulate == the T1 GPU path, bit-exact
    for (int i = 0; i < sn; ++i)
    {
        const float p = xv[i] * yv[i];
        facc          = facc + p;
        ref[i]        = facc;
    }
    const float* inputs[] = {xv, yv};
    float        gpu[sn];
    REQUIRE(vk.run_graph(g, s, inputs, 2, gpu));

    int mism = 0;
    for (int i = 0; i < sn; ++i) { if (gpu[i] != ref[i]) { ++mism; } }
    CHECK(mism == 0);
}

// ── v17-e: the FULL radix SORT as ONE multi-kernel GPU pipeline (the payoff — authored once in CKIR, run on the GPU) ──
TEST_CASE("v17-e: full radix sort runs on GPU as one multi-kernel pipeline (bit-exact vs std::sort)", "[kir][vulkan][gpu][radix][scheduler]")
{
    crd::memory::TlsfAllocator alloc(128U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    // Keys are F32 holding EXACT integer values (16-bit ⇒ ≤ 2^24, exact); only the transient bit-extraction casts to I32.
    // So scan/reduce/broadcast/scatter all run on the float emitters; the WHOLE 16-pass sort is one graph → one run_graph.
    kir::KGraph      g(&alloc);
    constexpr int    rn   = 256;
    constexpr int    bits = 16;
    const kir::Shape sh   = kir::make_shape({rn});
    int              k    = g.input(sh, kir::DType::F32);
    for (int bit = 0; bit < bits; ++bit)
    {
        const int ki      = g.cast(k, kir::DType::I32);
        const int shifted = g.binary(kir::KOp::Shr, ki, g.constant(static_cast<crd::f64>(bit), sh, kir::DType::I32));
        const int biti    = g.binary(kir::KOp::BitAnd, shifted, g.constant(1.0, sh, kir::DType::I32));
        const int bitf    = g.cast(biti, kir::DType::F32);
        const int f       = g.binary(kir::KOp::Sub, g.constant(1.0, sh, kir::DType::F32), bitf); // is-false predicate
        const int incl    = g.scan(f);
        const int e       = g.binary(kir::KOp::Sub, incl, f);
        const int tf      = g.broadcast(g.reduce(kir::KOp::ReduceSum, f, 1U), sh); // total falses, fanned out
        const int idx     = g.iota(sh, 0, kir::DType::F32);
        const int true_b  = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Add, tf, idx), e);
        const int pos     = g.select(bitf, true_b, e);
        k                 = g.scatter(k, pos, k); // permute keys to their split positions (stable)
    }

    crd::u32 orig[rn];
    float    kv[rn];
    for (int i = 0; i < rn; ++i)
    {
        orig[i] = static_cast<crd::u32>((i * 2654435761U) & 0xFFFFU); // 16-bit keys
        kv[i]   = static_cast<float>(orig[i]);
    }
    const float* inputs[] = {kv};
    float        gpu[rn];
    REQUIRE(vk.run_graph(g, k, inputs, 1, gpu));

    crd::u32 ref[rn];
    for (int i = 0; i < rn; ++i) { ref[i] = orig[i]; }
    std::sort(ref, ref + rn);

    int mism = 0;
    for (int i = 0; i < rn; ++i) { if (static_cast<crd::u32>(gpu[i]) != ref[i]) { ++mism; } }
    CHECK(mism == 0);
}

// ── v17 Phase A (ADR-0101): shader-intrinsic library on the GPU ─────────────────────────────────────────────────────
TEST_CASE("v17 Phase A: CKIR shader intrinsics (fract/step/clamp/mix bit-exact, pow ULP) on Vulkan", "[kir][vulkan][gpu][intrinsics]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    constexpr int    in = 256;
    const kir::Shape sh = kir::make_shape({in});
    float            xv[in];
    float            zv[in];
    float            wv[in];
    for (int i = 0; i < in; ++i)
    {
        xv[i] = (0.3F * static_cast<float>(i)) - 5.0F;
        zv[i] = (0.1F * static_cast<float>(i % 12)) - 0.5F;
        wv[i] = static_cast<float>(i % 16) / 16.0F; // dyadic ⇒ exact f32
    }

    SECTION("fract/step/clamp/mix — bit-exact")
    {
        kir::KGraph g(&alloc);
        const int   x  = g.input(sh, kir::DType::F32);
        const int   z  = g.input(sh, kir::DType::F32);
        const int   w  = g.input(sh, kir::DType::F32);
        const int   fr = g.unary(kir::KOp::Fract, x);
        const int   cl = g.ternary(kir::KOp::Clamp, fr, g.constant(0.2, sh, kir::DType::F32), g.constant(0.8, sh, kir::DType::F32));
        const int   st = g.binary(kir::KOp::Step, g.constant(0.5, sh, kir::DType::F32), z);
        const int   mx = g.ternary(kir::KOp::Mix, cl, st, w);

        float ref[in];
        for (int i = 0; i < in; ++i)
        {
            const float fr_r = xv[i] - std::floor(xv[i]);
            const float m    = fr_r > 0.2F ? fr_r : 0.2F;
            const float cl_r = m < 0.8F ? m : 0.8F;
            const float st_r = zv[i] < 0.5F ? 0.0F : 1.0F;
            ref[i]           = (cl_r * (1.0F - wv[i])) + (st_r * wv[i]); // MSVC /fp:precise ⇒ no FMA ⇒ matches precise GLSL
        }
        const float* inputs[] = {xv, zv, wv};
        float        gpu[in];
        REQUIRE(vk.run(g, mx, inputs, 3, gpu));
        int mism = 0;
        for (int i = 0; i < in; ++i) { if (gpu[i] != ref[i]) { ++mism; } }
        CHECK(mism == 0);
    }

    SECTION("pow — within ULP tolerance (transcendental)")
    {
        kir::KGraph g(&alloc);
        const int   x  = g.input(sh, kir::DType::F32);
        const int   pw = g.binary(kir::KOp::Pow, x, g.constant(3.0, sh, kir::DType::F32));
        float       xp[in];
        for (int i = 0; i < in; ++i) { xp[i] = 0.1F + (0.02F * static_cast<float>(i)); } // positive base
        const float* inputs[] = {xp};
        float        gpu[in];
        REQUIRE(vk.run(g, pw, inputs, 1, gpu));
        int bad = 0;
        for (int i = 0; i < in; ++i)
        {
            const float r = std::pow(xp[i], 3.0F);
            if (std::fabs(gpu[i] - r) > (1e-4F * std::fabs(r)) + 1e-6F) { ++bad; }
        }
        CHECK(bad == 0);
    }
}

// ── v17 A4: fixed-count loops (unroll_for) fan out to the GPU for free (pure dataflow) ───────────────────────────────
TEST_CASE("v17 A4: CKIR unroll_for (fixed-count loop) runs on Vulkan (bit-exact)", "[kir][vulkan][gpu][controlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }
    constexpr int    cn = 256;
    const kir::Shape sh = kir::make_shape({cn});
    kir::KGraph      g(&alloc);
    const int        x = g.input(sh, kir::DType::F32);
    const int        y = g.input(sh, kir::DType::F32);
    const int        r = g.unroll_for(8, x, [&](int i, int acc) { return g.binary(kir::KOp::Add, acc, g.binary(kir::KOp::Mul, g.constant(static_cast<crd::f64>(i), sh, kir::DType::F32), y)); });

    float xv[cn];
    float yv[cn];
    for (int i = 0; i < cn; ++i) { const float fi = static_cast<float>(i); xv[i] = (0.05F * fi) - 3.0F; yv[i] = 0.1F + (0.003F * fi); }
    const float* inp[] = {xv, yv};
    float        gpu[cn];
    REQUIRE(vk.run(g, r, inp, 2, gpu));

    int mism = 0;
    for (int i = 0; i < cn; ++i) { float acc = xv[i]; for (int it = 0; it < 8; ++it) { acc = acc + (static_cast<float>(it) * yv[i]); } if (gpu[i] != acc) { ++mism; } }
    CHECK(mism == 0);
}

TEST_CASE("v17 A4 tier-2: CKIR dynamic for_loop (native GPU loop, index + divergent count) on Vulkan", "[kir][vulkan][gpu][controlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }
    constexpr int    cn  = 128;
    const kir::Shape sh  = kir::make_shape({cn});
    kir::KGraph      g(&alloc);
    const int        x   = g.input(sh, kir::DType::F32);
    const int        y   = g.input(sh, kir::DType::F32);
    const int        cnt = g.input(sh, kir::DType::F32);
    // acc = x; for it in [0, cnt): acc = acc + it*y   (uses the loop index + a divergent per-element count)
    const int r = g.for_loop(cnt, x, [&](int idx, int acc) { return g.binary(kir::KOp::Add, acc, g.binary(kir::KOp::Mul, idx, y)); });

    float xv[cn];
    float yv[cn];
    float cv[cn];
    for (int i = 0; i < cn; ++i) { const float fi = static_cast<float>(i); xv[i] = (0.05F * fi) - 3.0F; yv[i] = 0.1F + (0.003F * fi); cv[i] = static_cast<float>(i % 8); }
    const float* inp[] = {xv, yv, cv};
    float        gpu[cn];
    REQUIRE(vk.run(g, r, inp, 3, gpu));

    int mism = 0;
    for (int i = 0; i < cn; ++i)
    {
        float     acc = xv[i];
        const int c   = static_cast<int>(cv[i]);
        for (int it = 0; it < c; ++it) { acc = acc + (static_cast<float>(it) * yv[i]); }
        if (gpu[i] != acc) { ++mism; }
    }
    CHECK(mism == 0);
}

// ── v17 A3: the comps-aware VEC/MAT emitter on the GPU (the whole vec/mat corpus now runs, not just the oracle) ────────
TEST_CASE("v17 A3: CKIR vec3 ops (construct/cross/add/normalize) run on Vulkan via the comps-aware emitter", "[kir][vulkan][gpu][vec]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    constexpr int    vn = 128;
    const kir::Shape sh = kir::make_shape({vn});
    kir::KGraph      g(&alloc);
    const int        ax = g.input(sh, kir::DType::F32);
    const int        ay = g.input(sh, kir::DType::F32);
    const int        az = g.input(sh, kir::DType::F32);
    const int        bx = g.input(sh, kir::DType::F32);
    const int        by = g.input(sh, kir::DType::F32);
    const int        bz = g.input(sh, kir::DType::F32);
    const int        a  = g.vec3(ax, ay, az);
    const int        b  = g.vec3(bx, by, bz);
    const int        o  = g.normalize(g.binary(kir::KOp::Add, g.cross(a, b), a)); // vec3 out

    float fin[6][vn];
    for (int i = 0; i < vn; ++i) { const float fi = static_cast<float>(i); fin[0][i] = 0.5F + 0.03F * fi; fin[1][i] = 1.0F - 0.02F * fi; fin[2][i] = 0.2F + 0.01F * fi; fin[3][i] = -0.4F + 0.02F * fi; fin[4][i] = 0.7F + 0.015F * fi; fin[5][i] = 0.9F - 0.01F * fi; }
    const float* inp[] = {fin[0], fin[1], fin[2], fin[3], fin[4], fin[5]};
    float        gpu[vn * 3];
    REQUIRE(vk.run(g, o, inp, 6, gpu));

    int bad = 0;
    for (int i = 0; i < vn; ++i)
    {
        const float avx = fin[0][i]; const float avy = fin[1][i]; const float avz = fin[2][i];
        const float bvx = fin[3][i]; const float bvy = fin[4][i]; const float bvz = fin[5][i];
        const float cx = avy * bvz - avz * bvy; const float cy = avz * bvx - avx * bvz; const float cz = avx * bvy - avy * bvx;
        const float sx = cx + avx; const float sy = cy + avy; const float sz = cz + avz;
        const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
        const float rx = sx / len; const float ry = sy / len; const float rz = sz / len;
        if (std::fabs(gpu[i * 3] - rx) > 1e-4F * std::fabs(rx) + 1e-5F) { ++bad; }
        if (std::fabs(gpu[i * 3 + 1] - ry) > 1e-4F * std::fabs(ry) + 1e-5F) { ++bad; }
        if (std::fabs(gpu[i * 3 + 2] - rz) > 1e-4F * std::fabs(rz) + 1e-5F) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("v17 A3: CKIR mat4 construct + quaternions/slerp + any/all run on Vulkan", "[kir][vulkan][gpu][vec]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }
    constexpr int    vn = 64;
    const kir::Shape sh  = kir::make_shape({vn});
    int              bad = 0;

    { // mat4 construct (MatFromCols4 via the 4th operand) + mat*vec4
        kir::KGraph g(&alloc);
        const int   c0 = g.input_vec(sh, kir::DType::F32, 4);
        const int   c1 = g.input_vec(sh, kir::DType::F32, 4);
        const int   c2 = g.input_vec(sh, kir::DType::F32, 4);
        const int   c3 = g.input_vec(sh, kir::DType::F32, 4);
        const int   v  = g.input_vec(sh, kir::DType::F32, 4);
        const int   mv = g.mat_mul_vec(g.mat4(c0, c1, c2, c3), v);
        float       cd[5][vn * 4];
        for (int i = 0; i < vn; ++i) { for (int k = 0; k < 4; ++k) { for (int col = 0; col < 5; ++col) { cd[col][i * 4 + k] = 0.5F + 0.1F * static_cast<float>(col) + 0.03F * static_cast<float>(i) + 0.2F * static_cast<float>(k); } } }
        const float* inp[] = {cd[0], cd[1], cd[2], cd[3], cd[4]};
        float        gpu[vn * 4];
        REQUIRE(vk.run(g, mv, inp, 5, gpu));
        for (int i = 0; i < vn; ++i) { for (int r = 0; r < 4; ++r) { float ref = 0.0F; for (int col = 0; col < 4; ++col) { ref += cd[col][i * 4 + r] * cd[4][i * 4 + col]; } if (std::fabs(gpu[i * 4 + r] - ref) > 1e-4F * std::fabs(ref) + 1e-4F) { ++bad; } } }
    }
    { // quat: rotate(q,v) ≡ quat_to_mat3(q)*v ; and slerp(q,q,0.5) ≡ q ; and any/all
        kir::KGraph g(&alloc);
        const int   axx = g.input(sh, kir::DType::F32);
        const int   axy = g.input(sh, kir::DType::F32);
        const int   axz = g.input(sh, kir::DType::F32);
        const int   ang = g.input(sh, kir::DType::F32);
        const int   vx = g.input(sh, kir::DType::F32);
        const int   vy = g.input(sh, kir::DType::F32);
        const int   vz = g.input(sh, kir::DType::F32);
        const int   q  = g.quat_axis_angle(g.normalize(g.vec3(axx, axy, axz)), ang);
        const int   v  = g.vec3(vx, vy, vz);
        const int   qr = g.quat_rotate(q, v);
        const int   mv = g.mat_mul_vec(g.quat_to_mat3(q), v);
        const int   sl = g.slerp(q, q, g.constant(0.5, sh, kir::DType::F32));
        const int   an = g.vany(v);
        float       in7[7][vn];
        for (int i = 0; i < vn; ++i) { const float fi = static_cast<float>(i); in7[0][i] = 0.3F + 0.1F * static_cast<float>(i % 4); in7[1][i] = 1.0F - 0.02F * fi; in7[2][i] = 0.2F + 0.03F * fi; in7[3][i] = 0.2F + 0.25F * fi; in7[4][i] = 0.5F * fi - 2.0F; in7[5][i] = 1.0F + 0.1F * fi; in7[6][i] = -0.3F * fi + 1.0F; }
        const float* inp[] = {in7[0], in7[1], in7[2], in7[3], in7[4], in7[5], in7[6]};
        float        gqr[vn * 3];
        float        gmv[vn * 3];
        float        gsl[vn * 4];
        float        gan[vn];
        REQUIRE(vk.run(g, qr, inp, 7, gqr));
        REQUIRE(vk.run(g, mv, inp, 7, gmv));
        REQUIRE(vk.run(g, sl, inp, 4, gsl)); // slerp reaches only axis+angle (q); v unused
        REQUIRE(vk.run(g, an, inp, 3, gan));
        for (int i = 0; i < vn * 3; ++i) { if (std::fabs(gqr[i] - gmv[i]) > 1e-3F * std::fabs(gmv[i]) + 1e-4F) { ++bad; } } // rotate ≡ mat·v
        for (int i = 0; i < vn; ++i) { if (gan[i] != 1.0F) { ++bad; } }                                                    // any(nonzero v)=1
    }
    CHECK(bad == 0);
}

TEST_CASE("v17 A3: CKIR mat3 (MatFromCols + mat*vec + inverse) runs on Vulkan via the comps-aware emitter", "[kir][vulkan][gpu][mat]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    constexpr int    mn = 96;
    const kir::Shape sh = kir::make_shape({mn});
    kir::KGraph      g(&alloc);
    const int        c0 = g.input_vec(sh, kir::DType::F32, 3); // 3 vec3 columns + a vec3 vector — all vec inputs
    const int        c1 = g.input_vec(sh, kir::DType::F32, 3);
    const int        c2 = g.input_vec(sh, kir::DType::F32, 3);
    const int        vv = g.input_vec(sh, kir::DType::F32, 3);
    const int        mat = g.mat3(c0, c1, c2);
    const int        mv  = g.mat_mul_vec(mat, vv);                               // vec3
    const int        pv  = g.mat_mul_vec(g.mat_mul(mat, g.mat_inverse(mat)), vv); // (M·M⁻¹)·v ≈ v

    float c0d[mn * 3];
    float c1d[mn * 3];
    float c2d[mn * 3];
    float vd[mn * 3];
    for (int i = 0; i < mn; ++i)
    {
        // diagonally-dominant columns ⇒ well-conditioned (invertible)
        const float fi = static_cast<float>(i);
        c0d[i * 3] = 3.0F + 0.01F * fi; c0d[i * 3 + 1] = 0.2F; c0d[i * 3 + 2] = 0.1F;
        c1d[i * 3] = 0.1F; c1d[i * 3 + 1] = 4.0F - 0.01F * fi; c1d[i * 3 + 2] = 0.2F;
        c2d[i * 3] = 0.2F; c2d[i * 3 + 1] = 0.1F; c2d[i * 3 + 2] = 5.0F + 0.005F * fi;
        vd[i * 3] = 0.5F * fi - 2.0F; vd[i * 3 + 1] = 1.0F + 0.1F * fi; vd[i * 3 + 2] = -0.3F * fi + 1.0F;
    }
    const float* inp[] = {c0d, c1d, c2d, vd};

    float gmv[mn * 3];
    float gpv[mn * 3];
    REQUIRE(vk.run(g, mv, inp, 4, gmv));
    REQUIRE(vk.run(g, pv, inp, 4, gpv));

    int bad = 0;
    for (int i = 0; i < mn; ++i)
    {
        for (int r = 0; r < 3; ++r)
        {
            const float ref = c0d[i * 3 + r] * vd[i * 3] + c1d[i * 3 + r] * vd[i * 3 + 1] + c2d[i * 3 + r] * vd[i * 3 + 2]; // column-major M·v
            if (std::fabs(gmv[i * 3 + r] - ref) > 1e-4F * std::fabs(ref) + 1e-4F) { ++bad; }        // mat·vec
            if (std::fabs(gpv[i * 3 + r] - vd[i * 3 + r]) > 1e-3F * std::fabs(vd[i * 3 + r]) + 1e-3F) { ++bad; } // (M·M⁻¹)·v ≈ v
        }
    }
    CHECK(bad == 0);
}

// D-007 B0-2: mat2 has comps == 4 exactly like a vec4, so a comps-keyed emitter would spell it `vec4` and index it
// `t[k]` instead of `t[col][row]`. Non-square exercises `matCxR` naming + the RxC outer product. Both are new capability.
TEST_CASE("v17 B0-2: CKIR mat2 (construct/mat*vec/inverse) + non-square 2x3 outer run on Vulkan", "[kir][vulkan][gpu][mat2]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    constexpr int    mn = 96;
    const kir::Shape sh = kir::make_shape({mn});
    kir::KGraph      g(&alloc);
    const int        c0 = g.input_vec(sh, kir::DType::F32, 2); // 2 vec2 columns + a vec2 vector
    const int        c1 = g.input_vec(sh, kir::DType::F32, 2);
    const int        vv = g.input_vec(sh, kir::DType::F32, 2);
    const int        mat = g.mat2(c0, c1);
    const int        mv  = g.mat_mul_vec(mat, vv);                                 // vec2
    const int        pv  = g.mat_mul_vec(g.mat_mul(mat, g.mat_inverse(mat)), vv);  // (M·M⁻¹)·v ≈ v

    float c0d[mn * 2];
    float c1d[mn * 2];
    float vd[mn * 2];
    for (int i = 0; i < mn; ++i)
    {
        const float fi = static_cast<float>(i);
        c0d[i * 2] = 3.0F + 0.01F * fi; c0d[i * 2 + 1] = 0.2F;                // diagonally dominant ⇒ invertible
        c1d[i * 2] = 0.1F;              c1d[i * 2 + 1] = 4.0F - 0.01F * fi;
        vd[i * 2]  = 0.5F * fi - 2.0F;  vd[i * 2 + 1]  = 1.0F + 0.1F * fi;
    }
    const float* inp[] = {c0d, c1d, vd};

    float gmv[mn * 2];
    float gpv[mn * 2];
    REQUIRE(vk.run(g, mv, inp, 3, gmv));
    REQUIRE(vk.run(g, pv, inp, 3, gpv));

    int bad = 0;
    for (int i = 0; i < mn; ++i)
    {
        for (int r = 0; r < 2; ++r)
        {
            const float ref = c0d[i * 2 + r] * vd[i * 2] + c1d[i * 2 + r] * vd[i * 2 + 1]; // column-major M·v
            if (std::fabs(gmv[i * 2 + r] - ref) > 1e-4F * std::fabs(ref) + 1e-4F) { ++bad; }
            if (std::fabs(gpv[i * 2 + r] - vd[i * 2 + r]) > 1e-3F * std::fabs(vd[i * 2 + r]) + 1e-3F) { ++bad; }
        }
    }
    CHECK(bad == 0);

    // non-square: vec2 (x) vec3 -> a 2-row x 3-col matrix, written back column-major (6 floats per element)
    kir::KGraph      g2(&alloc);
    const int        a2 = g2.input_vec(sh, kir::DType::F32, 2);
    const int        b3 = g2.input_vec(sh, kir::DType::F32, 3);
    const int        op = g2.outer_product(a2, b3);
    REQUIRE(g2.node(op).type.rows == 2);
    REQUIRE(g2.node(op).type.cols == 3);

    float ad[mn * 2];
    float bd[mn * 3];
    for (int i = 0; i < mn; ++i)
    {
        const float fi = static_cast<float>(i);
        ad[i * 2] = 0.5F * fi - 1.0F; ad[i * 2 + 1] = 2.0F - 0.1F * fi;
        bd[i * 3] = 1.0F + 0.2F * fi; bd[i * 3 + 1] = -0.3F * fi; bd[i * 3 + 2] = 0.75F;
    }
    const float* inp2[] = {ad, bd};
    float        gop[mn * 6];
    REQUIRE(vk.run(g2, op, inp2, 2, gop));

    int badop = 0;
    for (int i = 0; i < mn; ++i)
    {
        for (int col = 0; col < 3; ++col)
        {
            for (int r = 0; r < 2; ++r)
            {
                const float ref = ad[i * 2 + r] * bd[i * 3 + col];
                if (std::fabs(gop[i * 6 + col * 2 + r] - ref) > 1e-4F * std::fabs(ref) + 1e-4F) { ++badop; }
            }
        }
    }
    CHECK(badop == 0);

    // a mat2 INPUT: comps == 4 collides with vec4, so `input_mat` is the only way to feed one. It must construct as
    // `mat2(...)` and transpose as a matrix, not be silently read back as a vec4.
    kir::KGraph g3(&alloc);
    const int   min2 = g3.input_mat(sh, kir::DType::F32, 2, 2);
    const int   mt2  = g3.mat_transpose(min2);
    REQUIRE(g3.node(min2).type.kind == kir::TKind::Mat);
    REQUIRE(g3.node(min2).comps() == 4);

    float md[mn * 4];
    for (int i = 0; i < mn; ++i)
    {
        const float fi = static_cast<float>(i);
        md[i * 4] = 1.0F + fi; md[i * 4 + 1] = 2.0F - fi; md[i * 4 + 2] = 0.5F * fi; md[i * 4 + 3] = 3.0F;
    }
    const float* inp3[] = {md};
    float        gmt[mn * 4];
    REQUIRE(vk.run(g3, mt2, inp3, 1, gmt));

    int badmt = 0;
    for (int i = 0; i < mn; ++i)
    {
        for (int col = 0; col < 2; ++col) { for (int r = 0; r < 2; ++r) { if (gmt[i * 4 + col * 2 + r] != md[i * 4 + r * 2 + col]) { ++badmt; } } } // transpose is exact
    }
    CHECK(badmt == 0);
}

// D-007 B0-3 on the GPU. GLSL has no `<` on vectors: a componentwise compare must emit lessThan() and produce a bvec3,
// which any()/all() then consume. It also rejects `precise bvec3` and `float + bool` -- all of which a comps-keyed,
// float-only emitter would have gotten wrong. `ivec3` exercises the int-prefixed type name via Cast.
TEST_CASE("v17 B0-3: CKIR bvec3 (lessThan + any/all) and ivec3 (via cast) run on Vulkan", "[kir][vulkan][gpu][boolvec]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    constexpr int    bn = 128;
    const kir::Shape sh = kir::make_shape({bn});
    kir::KGraph      g(&alloc);
    const int        av = g.input_vec(sh, kir::DType::F32, 3);
    const int        bv = g.input_vec(sh, kir::DType::F32, 3);
    const int        lt = g.binary(kir::KOp::CmpLt, av, bv); // bvec3
    const int        an = g.vany(lt);                        // bool -> written as float 0/1
    const int        al = g.vall(lt);
    REQUIRE(g.node(lt).type.scalar == kir::DType::Bool);
    REQUIRE(g.node(lt).type.rows == 3);

    float avd[bn * 3];
    float bvd[bn * 3];
    for (int i = 0; i < bn; ++i)
    {
        const float fi = static_cast<float>(i);
        avd[i * 3] = fi - 64.0F;                  avd[i * 3 + 1] = 1.0F; avd[i * 3 + 2] = (i % 2 == 0) ? -1.0F : 5.0F;
        bvd[i * 3] = 0.0F;                        bvd[i * 3 + 1] = 2.0F; bvd[i * 3 + 2] = 0.0F;
    }
    const float* inp[] = {avd, bvd};
    float        gan[bn];
    float        gal[bn];
    REQUIRE(vk.run(g, an, inp, 2, gan));
    REQUIRE(vk.run(g, al, inp, 2, gal));

    int bad = 0;
    for (int i = 0; i < bn; ++i)
    {
        const bool eany = (avd[i * 3] < bvd[i * 3]) || (avd[i * 3 + 1] < bvd[i * 3 + 1]) || (avd[i * 3 + 2] < bvd[i * 3 + 2]);
        const bool eall = (avd[i * 3] < bvd[i * 3]) && (avd[i * 3 + 1] < bvd[i * 3 + 1]) && (avd[i * 3 + 2] < bvd[i * 3 + 2]);
        if (gan[i] != (eany ? 1.0F : 0.0F)) { ++bad; } // bool readback is exact, not tolerance-based
        if (gal[i] != (eall ? 1.0F : 0.0F)) { ++bad; }
    }
    CHECK(bad == 0);

    // ivec3: float vec3 -> ivec3 (truncates) -> componentwise add -> back to vec3
    kir::KGraph g2(&alloc);
    const int   fv  = g2.input_vec(sh, kir::DType::F32, 3);
    const int   ivv = g2.cast(fv, kir::DType::I32);
    const int   sum = g2.binary(kir::KOp::Add, ivv, ivv);
    const int   out = g2.cast(sum, kir::DType::F32);
    REQUIRE(g2.node(ivv).type == kir::KType::vec(kir::DType::I32, 3));

    float fvd[bn * 3];
    for (int i = 0; i < bn; ++i)
    {
        const float fi = static_cast<float>(i);
        fvd[i * 3] = fi * 0.5F - 10.0F; fvd[i * 3 + 1] = 3.25F; fvd[i * 3 + 2] = -2.75F;
    }
    const float* inp2[] = {fvd};
    float        giv[bn * 3];
    REQUIRE(vk.run(g2, out, inp2, 1, giv));

    int badi = 0;
    for (int i = 0; i < bn; ++i)
    {
        for (int k = 0; k < 3; ++k)
        {
            const float ref = static_cast<float>(2 * static_cast<int>(fvd[i * 3 + k])); // int truncation, then doubled
            if (giv[i * 3 + k] != ref) { ++badi; }
        }
    }
    CHECK(badi == 0);
}

// D-007 B0-4 on the GPU. A struct/array value is lowered by SROA — the aggregate is never materialized, a FieldGet
// resolves to the field's temp — so no GLSL `struct` declaration and no std430 layout appear. This is what Slang/DXC do
// for value-typed aggregates; buffer-BACKED structs (which do need std430) are the resource-binding slice, B3.
TEST_CASE("v17 B0-4: a Light struct + a vec3 array round-trip through the SROA lowering on Vulkan", "[kir][vulkan][gpu][aggregate]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }

    constexpr int    an = 96;
    const kir::Shape sh = kir::make_shape({an});
    kir::KGraph      g(&alloc);

    // struct Light { vec3 pos; float radius; vec3 color; }
    const kir::KType fields[3] = {kir::KType::vec(kir::DType::F32, 3), kir::KType::make_scalar(kir::DType::F32),
                                  kir::KType::vec(kir::DType::F32, 3)};
    const int        light = g.define_struct(fields, 3);
    REQUIRE(g.struct_flat_comps(light) == 7);

    const int pos = g.input_vec(sh, kir::DType::F32, 3);
    const int rad = g.input(sh, kir::DType::F32);
    const int col = g.input_vec(sh, kir::DType::F32, 3);
    const int fl[3] = {pos, rad, col};
    const int lite  = g.struct_make(light, fl, 3);
    // destructure, then recombine: color * radius + pos  (proves each field lands in the right slot)
    const int f_pos = g.field_get(lite, 0);
    const int f_rad = g.field_get(lite, 1);
    const int f_col = g.field_get(lite, 2);
    const int out   = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, f_col, g.splat(f_rad, 3)), f_pos);

    float posd[an * 3];
    float radd[an];
    float cold[an * 3];
    for (int i = 0; i < an; ++i)
    {
        const float fi = static_cast<float>(i);
        posd[i * 3] = fi * 0.5F - 8.0F; posd[i * 3 + 1] = 1.0F - 0.1F * fi; posd[i * 3 + 2] = 0.25F * fi;
        radd[i]     = 0.5F + 0.03F * fi;
        cold[i * 3] = 0.1F * fi;        cold[i * 3 + 1] = 2.0F;             cold[i * 3 + 2] = -0.4F * fi;
    }
    const float* inp[] = {posd, radd, cold};
    float        gout[an * 3];
    REQUIRE(vk.run(g, out, inp, 3, gout));

    int bad = 0;
    for (int i = 0; i < an; ++i)
    {
        for (int k = 0; k < 3; ++k)
        {
            const float ref = cold[i * 3 + k] * radd[i] + posd[i * 3 + k];
            if (std::fabs(gout[i * 3 + k] - ref) > 1e-4F * std::fabs(ref) + 1e-4F) { ++bad; }
        }
    }
    CHECK(bad == 0);

    // a fixed-size array of vec3: pick element 1 back out
    kir::KGraph g2(&alloc);
    const int   e0 = g2.input_vec(sh, kir::DType::F32, 3);
    const int   e1 = g2.input_vec(sh, kir::DType::F32, 3);
    const int   ev[2] = {e0, e1};
    const int   arr   = g2.array_make(ev, 2);
    REQUIRE(g2.node(arr).type.count == 2);
    REQUIRE(g2.node(arr).comps() == 6);
    const int   got = g2.array_get(arr, 1); // == e1
    REQUIRE(g2.node(got).comps() == 3);

    const float* inp2[] = {posd, cold};
    float        garr[an * 3];
    REQUIRE(vk.run(g2, got, inp2, 2, garr));

    int bada = 0;
    for (int i = 0; i < an * 3; ++i) { if (garr[i] != cold[i]) { ++bada; } } // an exact element read, no arithmetic
    CHECK(bada == 0);
}

TEST_CASE("v17 Phase A2: CKIR transcendental intrinsics (exp2/log2/rsqrt/tan/atan2/smoothstep/radians) on Vulkan", "[kir][vulkan][gpu][intrinsics]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendVulkan      vk(&alloc);
    if (!vk.valid()) { WARN("no Vulkan device available; skipping"); return; }
    constexpr int    in = 256;
    const kir::Shape sh = kir::make_shape({in});

    // one-input op vs a std reference, within ULP tolerance (transcendentals are ULP on GPU).
    auto check1 = [&](kir::KOp op, float (*ref)(float), float lo, float hi) -> int {
        kir::KGraph g(&alloc);
        const int   x = g.input(sh, kir::DType::F32);
        const int   o = g.unary(op, x);
        float       xv[in];
        for (int i = 0; i < in; ++i) { xv[i] = lo + ((hi - lo) * static_cast<float>(i) / static_cast<float>(in)); }
        const float* inp[] = {xv};
        float        gpu[in];
        if (!vk.run(g, o, inp, 1, gpu)) { return -1; }
        int bad = 0;
        for (int i = 0; i < in; ++i) { const float r = ref(xv[i]); if (std::fabs(gpu[i] - r) > (2e-4F * std::fabs(r)) + 1e-6F) { ++bad; } }
        return bad;
    };
    CHECK(check1(kir::KOp::Exp2, [](float v) { return std::exp2(v); }, 0.5F, 3.0F) == 0);
    CHECK(check1(kir::KOp::Log2, [](float v) { return std::log2(v); }, 0.5F, 8.0F) == 0);
    CHECK(check1(kir::KOp::Rsqrt, [](float v) { return 1.0F / std::sqrt(v); }, 0.5F, 8.0F) == 0);
    CHECK(check1(kir::KOp::Tan, [](float v) { return std::tan(v); }, -1.0F, 1.0F) == 0);
    CHECK(check1(kir::KOp::Radians, [](float v) { return v * 0.017453292519943295F; }, -180.0F, 180.0F) == 0);
    CHECK(check1(kir::KOp::Asin, [](float v) { return std::asin(v); }, -0.9F, 0.9F) == 0);
    CHECK(check1(kir::KOp::Acos, [](float v) { return std::acos(v); }, -0.9F, 0.9F) == 0);
    CHECK(check1(kir::KOp::Atan, [](float v) { return std::atan(v); }, -3.0F, 3.0F) == 0);
    CHECK(check1(kir::KOp::Sinh, [](float v) { return std::sinh(v); }, -2.0F, 2.0F) == 0);
    CHECK(check1(kir::KOp::Cosh, [](float v) { return std::cosh(v); }, -2.0F, 2.0F) == 0);
    CHECK(check1(kir::KOp::Cbrt, [](float v) { return std::cbrt(v); }, -8.0F, 8.0F) == 0);

    SECTION("atan2 (binary) + smoothstep (ternary)")
    {
        kir::KGraph g(&alloc);
        const int   y  = g.input(sh, kir::DType::F32);
        const int   x  = g.input(sh, kir::DType::F32);
        const int   at = g.binary(kir::KOp::Atan2, y, x);
        const int   ss = g.ternary(kir::KOp::Smoothstep, g.constant(-0.5, sh, kir::DType::F32), g.constant(2.5, sh, kir::DType::F32), y);
        float       yv[in];
        float       xv[in];
        for (int i = 0; i < in; ++i) { yv[i] = (0.03F * static_cast<float>(i)) - 3.0F; xv[i] = 1.0F + (0.02F * static_cast<float>(i)); }
        const float* inp[] = {yv, xv};
        float        gat[in];
        float        gss[in];
        REQUIRE(vk.run(g, at, inp, 2, gat));
        REQUIRE(vk.run(g, ss, inp, 1, gss)); // smoothstep uses only y (input 0); atan2 uses both
        int bad = 0;
        for (int i = 0; i < in; ++i)
        {
            const float ra = std::atan2(yv[i], xv[i]);
            const float u  = (yv[i] - (-0.5F)) / (2.5F - (-0.5F));
            const float hi = u > 1.0F ? 1.0F : u;
            const float t  = u < 0.0F ? 0.0F : hi;
            const float rs = t * t * (3.0F - (2.0F * t));
            if (std::fabs(gat[i] - ra) > (2e-4F * std::fabs(ra)) + 1e-6F) { ++bad; }
            if (std::fabs(gss[i] - rs) > (2e-4F * std::fabs(rs)) + 1e-6F) { ++bad; }
        }
        CHECK(bad == 0);
    }

    SECTION("mod (binary, ULP) + fma (ternary, bit-exact)")
    {
        kir::KGraph g(&alloc);
        const int   a  = g.input(sh, kir::DType::F32);
        const int   b  = g.input(sh, kir::DType::F32);
        const int   c  = g.input(sh, kir::DType::F32);
        const int   md = g.binary(kir::KOp::Mod, a, b);
        const int   fm = g.ternary(kir::KOp::Fma, a, b, c);
        float       av[in];
        float       bv[in];
        float       cv[in];
        for (int i = 0; i < in; ++i)
        {
            bv[i] = 3.0F;                                                                                    // nonzero divisor
            av[i] = (3.0F * static_cast<float>(i % 4)) + (0.5F + (1.5F * static_cast<float>(i % 3) / 3.0F)); // a/b fractional ∈ [.16,.66] ⇒ trunc stable
            cv[i] = (0.01F * static_cast<float>(i)) - 1.0F;
        }
        const float* inp[] = {av, bv, cv};
        float        gmd[in];
        float        gfm[in];
        REQUIRE(vk.run(g, md, inp, 2, gmd)); // mod uses a,b
        REQUIRE(vk.run(g, fm, inp, 3, gfm)); // fma uses a,b,c
        int bad = 0;
        for (int i = 0; i < in; ++i)
        {
            const float rm = std::fmod(av[i], bv[i]);
            const float rf = std::fma(av[i], bv[i], cv[i]);
            if (std::fabs(gmd[i] - rm) > (2e-4F * std::fabs(bv[i])) + 1e-5F) { ++bad; } // ULP (division-based; float-mod boundary caveat)
            if (gfm[i] != rf) { ++bad; }                                               // IEEE single-round fma ⇒ bit-exact
        }
        CHECK(bad == 0);
    }
}
