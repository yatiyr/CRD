// CEIR-13z-1b (Vulkan) — a CEIR `add` compute ASSET, lowered by 13d and run on a real Vulkan device through
// `execute_lowered` (ADR-0126), proven BYTE-IDENTICAL to the direct CKIR dispatch of the SAME kernel + == the CPU oracle,
// validation-SILENT. ⛔ The device-free "always-runs" case (the asset lowers to one dispatch) guards the all-skip false-green
// for THIS target; the device case soft-skips with no adapter. The seam itself is unit-tested device-free in
// tests/ceir-gpu/test_execute.cpp; here it drives a real GPU.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/parse.hpp> // CEIR-13z-2 §121: parse a TEXT-authored asset
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_fft.hpp>    // CEIR-13z-3: build_fft2d_c2c (the 6-dispatch 2D FFT plan)
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_reduce.hpp> // CEIR-13z-2: build_reduce_block (the single-workgroup device reduction)
#include <crd/kir/ckir_scan.hpp>   // CEIR-13z-2: build_scan_block (the single-workgroup device prefix sum)

#include <crd/math/cmath.hpp> // CEIR-13z-3: cos/sin for the FFT twiddle tables

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ceir_execute_1wg.hpp"
#include "../gpu-shared/ckir_kernel_dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace cgt = crd::ceir_gpu_test;

TEST_CASE("ceir 13z: the add CEIR asset lowers to one dispatch over 3 bindings (device-free always-runs)", "[ceir][ceir-gpu][vulkan]")
{
    crd::memory::MallocAllocator root;
    ce::Context                  cctx(&root);
    const ce::Value*             binds[3];
    ce::Block* const             b = cgt::build_add_ceir_asset(cctx, binds);

    crd::containers::Array<ceg::LoweredCommand> cmds(&root);
    ceg::lower_region(cctx, *b, cmds);
    REQUIRE(cmds.size() == 1U);
    CHECK(cmds[0].kind == ceg::LoweredKind::Dispatch);
    CHECK_FALSE(cmds[0].dynamic_grid);
    CHECK(cmds[0].groups_x == 1U); // grid const = 1 workgroup (the ls threads live in local_size)
    CHECK(binds[0] != nullptr);
    CHECK(binds[2] != nullptr);
}

TEST_CASE("ceir 13z: add CEIR asset on Vulkan == direct CKIR (byte-identical) + oracle + validation-silent", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true; // the dispatch path must be validation-SILENT, asserted by counters
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    // build + emit + compile the add kernel to a pipeline (3 storage bindings, no push constant)
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = cgt::build_add_kernel(g, n);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_add", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    // build + lower the CEIR asset
    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const ce::Value*             binds[3];
    ce::Block* const             blk = cgt::build_add_ceir_asset(cctx, binds);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *blk, cmds);
    REQUIRE(cmds.size() == 1U);

    // inputs (f32 add is one IEEE op -> bit-exact on every backend, llvmpipe included)
    float a0[n];
    float b0[n];
    for (int i = 0; i < n; ++i)
    {
        a0[i] = static_cast<float>(i) * 0.5F;
        b0[i] = static_cast<float>(n - i) * 0.25F;
    }
    const int lens[3] = {n, n, n};

    // DIRECT path: dispatch_kernel_1wg (rec.dispatch) — the reference.
    float ad[n];
    float bd[n];
    float cd[n];
    for (int i = 0; i < n; ++i) { ad[i] = a0[i]; bd[i] = b0[i]; cd[i] = 0.0F; }
    float* hd[3] = {ad, bd, cd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 3, 1U);

    // CEIR path: dispatch_ceir_1wg (execute_lowered) — the SAME pipe, separate output.
    float ac[n];
    float bc[n];
    float cc[n];
    for (int i = 0; i < n; ++i) { ac[i] = a0[i]; bc[i] = b0[i]; cc[i] = 0.0F; }
    float* hc[3] = {ac, bc, cc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), binds, *pipe,
                               compute, hc, lens, 3, 1U);

    CHECK(err == ceg::ExecuteError::None);
    CHECK(std::memcmp(cc, cd, sizeof(cc)) == 0);        // ⭐ CEIR path byte-identical to the direct CKIR path
    for (int i = 0; i < n; ++i) { CHECK(cc[i] == a0[i] + b0[i]); } // == the CPU oracle (f32 add exact)
    CHECK(capture.error_count() == 0U);                 // validation-SILENT
}

TEST_CASE("ceir 13z: reduce CEIR asset on Vulkan == direct CKIR (byte-identical) + oracle + validation-silent", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true;
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    // build_reduce_block: ONE workgroup of n threads sum-reduces its span of n elements -> out[0]. 2 buffers (in r, out w).
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_reduce_block(g, n, n, crd::kir::KOp::Add);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_reduce", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDispatchAsset asset = cgt::build_ceir_dispatch_asset(cctx, "reduce", "r,w", 2);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    REQUIRE(cmds.size() == 1U);

    // integer-valued f32 inputs (sum 0+1+..+63 = 2016 < 2^24 -> every association order is EXACT, so the naive oracle is `==`)
    float in0[n];
    float expect = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        expect += in0[i];
    }
    const int lens[2] = {n, 1}; // out is a single reduced value (one workgroup -> out[0])

    float ind[n];
    float outd[1];
    for (int i = 0; i < n; ++i) { ind[i] = in0[i]; }
    outd[0]      = 0.0F;
    float* hd[2] = {ind, outd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 2, 1U);

    float inc[n];
    float outc[1];
    for (int i = 0; i < n; ++i) { inc[i] = in0[i]; }
    outc[0]      = 0.0F;
    float* hc[2] = {inc, outc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), asset.binds,
                               *pipe, compute, hc, lens, 2, 1U);

    CHECK(err == ceg::ExecuteError::None);
    CHECK(outc[0] == outd[0]);          // ⭐ CEIR path byte-identical to the direct CKIR path
    CHECK(outc[0] == expect);           // == the CPU oracle (integer-valued -> exact)
    CHECK(capture.error_count() == 0U); // validation-SILENT
}

TEST_CASE("ceir 13z: scan CEIR asset on Vulkan == direct CKIR (byte-identical) + oracle + validation-silent", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true;
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    // build_scan_block: ONE workgroup INCLUSIVE-scans its span in place; write_blocksum=TRUE publishes the span TOTAL to
    // bsum[0] (⛔ landmine 2: every declared buffer LIVE). 3 buffers: in(r), out(w), bsum(w).
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_scan_block(g, n, n, /*inclusive=*/true, /*write_blocksum=*/true);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_scan", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDispatchAsset asset = cgt::build_ceir_dispatch_asset(cctx, "scan", "r,w,w", 3);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    REQUIRE(cmds.size() == 1U);

    // integer-valued inputs -> the inclusive prefix sums k(k+1)/2 are all exact integers < 2^24.
    float in0[n];
    float pref[n];
    float total = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        total += in0[i];
        pref[i] = total;
    }
    const int lens[3] = {n, n, 1}; // in, out (scanned), bsum (the span total)

    float ind[n];
    float outd[n];
    float bsd[1];
    for (int i = 0; i < n; ++i)
    {
        ind[i]  = in0[i];
        outd[i] = 0.0F;
    }
    bsd[0]       = 0.0F;
    float* hd[3] = {ind, outd, bsd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 3, 1U);

    float inc[n];
    float outc[n];
    float bsc[1];
    for (int i = 0; i < n; ++i)
    {
        inc[i]  = in0[i];
        outc[i] = 0.0F;
    }
    bsc[0]       = 0.0F;
    float* hc[3] = {inc, outc, bsc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), asset.binds,
                               *pipe, compute, hc, lens, 3, 1U);

    CHECK(err == ceg::ExecuteError::None);
    CHECK(std::memcmp(outc, outd, sizeof(outc)) == 0); // ⭐ CEIR path byte-identical to the direct CKIR path
    CHECK(bsc[0] == bsd[0]);
    for (int i = 0; i < n; ++i) { CHECK(outc[i] == pref[i]); } // == the inclusive-scan oracle
    CHECK(bsc[0] == total);                                    // bsum[0] == the span total (the free landmine-2 oracle)
    CHECK(capture.error_count() == 0U);                        // validation-SILENT
}

TEST_CASE("ceir 13z: a TEXT-authored reduce asset (parsed) executes on Vulkan == oracle (the text-authoring device leg)", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_reduce_block(g, n, n, crd::kir::KOp::Add);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_reduce_txt", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    // ⭐ the asset is authored as TEXT and PARSED (not built) — the §121 authoring path reaching real GPU execution.
    const char* const k_reduce_text = "module {\n"
                                      "  ^bb0:\n"
                                      "    %0 = arith.const() {value = 1} : !index\n"
                                      "    %1 = resource.declare() : !buffer<plain,!f32>\n"
                                      "    %2 = resource.declare() : !buffer<plain,!f32>\n"
                                      "    compute.dispatch(%0, %0, %0, %1, %2) {access = \"r,w\", kernel = @reduce}\n"
                                      "}\n";
    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const ce::ParseResult        pr = ce::parse(cctx, crd::containers::StringView(k_reduce_text));
    REQUIRE(pr.ok);
    ce::Block* const pb = pr.module->body()->first_block();
    REQUIRE(pb != nullptr);
    const ce::Value* binds[8];
    const int        nb = cgt::collect_dispatch_binds(cctx, *pb, binds);
    REQUIRE(nb == 2);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *pb, cmds);
    REQUIRE(cmds.size() == 1U);

    float in0[n];
    float expect = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        expect += in0[i];
    }
    const int lens[2] = {n, 1};
    float     inc[n];
    float     outc[1];
    for (int i = 0; i < n; ++i) { inc[i] = in0[i]; }
    outc[0]      = 0.0F;
    float* hc[2] = {inc, outc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), binds, *pipe,
                               compute, hc, lens, 2, 1U);
    CHECK(err == ceg::ExecuteError::None);
    CHECK(outc[0] == expect); // the TEXT-authored reduce ran on the GPU and == the oracle
}

// ⭐ CEIR-13z-3 part 3: the 6-dispatch 2D FFT as a MULTI-DISPATCH CEIR asset — the on-device BARRIER STRESS. The CEIR
// lowering derives the per-resource inter-pass barriers (⭐ pass 3 gets TWO: RAW on b_tr_re from pass 1 AND b_tr_im from
// pass 2 — the part-1 completion, load-bearing here) and execute_lowered replays them. Bit-exact vs the CPU oracle
// (run_fft2d_cpu) — the SAME correctness gate the direct dispatch_fft2d test uses.
TEST_CASE("ceir 13z: a 6-dispatch 2D FFT CEIR asset on Vulkan == CPU oracle (multi-dispatch per-resource barriers)", "[ceir][ceir-gpu][vulkan][gpu][fft]")
{
    namespace gpu = crd::gpu;
    namespace kir = crd::kir;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true;
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(64U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    kir::KGraph          g0(&alloc);
    kir::KGraph          g1(&alloc);
    kir::KGraph          g2(&alloc);
    kir::KGraph          g3(&alloc);
    kir::KGraph*         graphs[4] = {&g0, &g1, &g2, &g3};
    constexpr int        rr   = 64;
    constexpr int        cc   = 64;
    constexpr int        tile = 16;
    const kir::Fft2dPlan plan = kir::build_fft2d_c2c(graphs, rr, cc, false, tile);

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[16];
    float*    h32[16];
    for (int b = 0; b < plan.nbuffers; ++b)
    {
        h64[b] = a64.data() + off[b];
        h32[b] = a32.data() + off[b];
    }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rr * cc; ++i)
    {
        h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
        h64[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 7 - 3);
    }
    for (int k = 0; k < cc; ++k)
    {
        const crd::f64 a         = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc);
        h64[plan.tw_col_re][k]   = f32d(crd::math::cos(a));
        h64[plan.tw_col_im][k]   = f32d(-crd::math::sin(a));
    }
    for (int k = 0; k < rr; ++k)
    {
        const crd::f64 a         = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr);
        h64[plan.tw_row_re][k]   = f32d(crd::math::cos(a));
        h64[plan.tw_row_im][k]   = f32d(-crd::math::sin(a));
    }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc); // the CPU oracle (mutates h64 -> the spectrum)

    // compile one pipeline per pass (emit GLSL -> SPIR-V -> pipeline; nbind per pass)
    std::unique_ptr<gpu::ComputePipeline> pipe_store[8];
    gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_fft", &alloc);
        REQUIRE(spv.ok);
        pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                                            plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    // Build the CEIR asset MIRRORING the plan: one dispatch per pass, honest access (FFT pass = r,r,r,r,w,w; transpose = r,w).
    cgt::MultiPass mp[8];
    const char*    names[8] = {"p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7"};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        mp[pi].kernel = names[pi];
        mp[pi].nbind  = plan.passes[pi].nbind;
        mp[pi].access = (plan.passes[pi].nbind == 6) ? "r,r,r,r,w,w" : "r,w";
        mp[pi].grid   = static_cast<int>(plan.passes[pi].num_workgroups);
        for (int k = 0; k < plan.passes[pi].nbind; ++k) { mp[pi].bind[k] = plan.passes[pi].bind[k]; }
    }
    crd::memory::MallocAllocator  croot;
    ce::Context                   cctx(&croot);
    const cgt::CeirMultiAsset     asset = cgt::build_ceir_multi_asset(cctx, plan.nbuffers, mp, plan.npasses);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);

    int sizes[16];
    for (int b = 0; b < plan.nbuffers; ++b) { sizes[b] = plan.buffers[b].size; }

    // run the CEIR path (execute_lowered derives + replays the inter-pass barriers) into h32.
    const ceg::ExecuteError err = cgt::dispatch_ceir_multi(
        cctx, asset, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), sizes, pipes, compute, h32);
    CHECK(err == ceg::ExecuteError::None);

    // bit-exact vs the CPU oracle (the same gate the direct dispatch_fft2d test uses).
    int badr = 0;
    int badi = 0;
    for (int i = 0; i < rr * cc; ++i)
    {
        if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
        if (h32[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
    }
    CHECK(badr == 0);
    CHECK(badi == 0);
    CHECK(capture.error_count() == 0U); // the multi-pass barriered execution is validation-SILENT
}
