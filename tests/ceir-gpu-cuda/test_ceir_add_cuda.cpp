// CEIR-13z (CUDA) — the THIRD backend: CEIR compute ASSETS (add/reduce/scan) executed on a real CUDA device via
// execute_lowered (ADR-0126), byte-identical to the direct CKIR dispatch. The `dispatch_ceir_1wg` harness is backend-agnostic
// (an IComputeContext&); only the context factory + emitter differ from Vulkan/DX12: emit_compute_kernel_cuda → NVRTC via
// create_pipeline_from_cuda (entry "ckir", + the kernel's local_size). ⛔ ValidationCapture-silent is Vulkan-only.
// ⭐ The FFT test below CAUGHT + DROVE a real CUDA-backend fix (2026-08-10): the CUDA context launched a FIXED 256-thread
// block, but shared-memory CKIR kernels (FFT/transpose) need blockDim.x == local_size — the multi-pass FFT was GARBAGE
// (maxrel=1) + the direct dispatch_fft2d SEGFAULTED. Fixed by threading the kernel's local_size through
// `create_pipeline_from_cuda` → `cuLaunchKernel`'s blockDim (engine/gpu-context-cuda). ⛔ single-workgroup add/reduce/scan
// PASSED even under the mismatch (reduce/scan legitimately, via identity-padded guarded threads; add by benign luck) — now
// correctly launched with their real local_size. ⭐ The CKIR kernels pass `fmad=false` (the per-kernel FP-contraction choice
// — user decision) so the FFT is BIT-EXACT vs the per-op oracle; the strict claim is also CEIR-path == direct-path
// byte-identical. (An elementwise/perf kernel would pass `fmad=true`, e.g. gpu-context-cuda's vecadd.)

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cuda.hpp>   // CEIR-13z (CUDA): emit_compute_kernel_cuda (KGraph -> CUDA C source, entry "ckir")
#include <crd/kir/ckir_fft.hpp>    // build_fft2d_c2c
#include <crd/kir/ckir_reduce.hpp> // build_reduce_block
#include <crd/kir/ckir_scan.hpp>   // build_scan_block

#include <crd/gpu/cuda_compute_context.hpp> // create_cuda_compute_context / CudaComputeContext / create_pipeline_from_cuda

#include <crd/math/cmath.hpp> // FFT twiddle tables

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ceir_execute_1wg.hpp"
#include "../gpu-shared/ckir_kernel_dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace cgt = crd::ceir_gpu_test;

TEST_CASE("ceir 13z: the add CEIR asset lowers to one dispatch over 3 bindings (cuda, device-free always-runs)", "[ceir][ceir-gpu][cuda]")
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
    CHECK(cmds[0].groups_x == 1U);
    CHECK(binds[0] != nullptr);
    CHECK(binds[2] != nullptr);
}

TEST_CASE("ceir 13z: add CEIR asset on CUDA == direct CKIR (byte-identical) + oracle", "[ceir][ceir-gpu][cuda][gpu]")
{
    namespace gpu = crd::gpu;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(alloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping"); return; }
    gpu::CudaComputeContext& compute = *cudactx;

    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = cgt::build_add_kernel(g, n);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_cuda(g, e, &alloc, kern));
    auto pipe = compute.create_pipeline_from_cuda(crd::containers::to_view(kern.source), crd::containers::StringView("ckir"), 3, e.local_size[0], 0U, /*fmad*/ false);
    REQUIRE(pipe != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const ce::Value*             binds[3];
    ce::Block* const             blk = cgt::build_add_ceir_asset(cctx, binds);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *blk, cmds);
    REQUIRE(cmds.size() == 1U);

    float a0[n];
    float b0[n];
    for (int i = 0; i < n; ++i)
    {
        a0[i] = static_cast<float>(i) * 0.5F;
        b0[i] = static_cast<float>(n - i) * 0.25F;
    }
    const int lens[3] = {n, n, n};

    float ad[n];
    float bd[n];
    float cd[n];
    for (int i = 0; i < n; ++i) { ad[i] = a0[i]; bd[i] = b0[i]; cd[i] = 0.0F; }
    float* hd[3] = {ad, bd, cd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 3, 1U);

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
}

TEST_CASE("ceir 13z: reduce CEIR asset on CUDA == direct CKIR (byte-identical) + oracle", "[ceir][ceir-gpu][cuda][gpu]")
{
    namespace gpu = crd::gpu;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(alloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping"); return; }
    gpu::CudaComputeContext& compute = *cudactx;

    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_reduce_block(g, n, n, crd::kir::KOp::Add);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_cuda(g, e, &alloc, kern));
    auto pipe = compute.create_pipeline_from_cuda(crd::containers::to_view(kern.source), crd::containers::StringView("ckir"), 2, e.local_size[0], 0U, /*fmad*/ false);
    REQUIRE(pipe != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDispatchAsset asset = cgt::build_ceir_dispatch_asset(cctx, "reduce", "r,w", 2);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    REQUIRE(cmds.size() == 1U);

    float in0[n];
    float expect = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        expect += in0[i];
    }
    const int lens[2] = {n, 1};

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
    CHECK(outc[0] == outd[0]); // ⭐ CEIR path byte-identical to the direct CKIR path
    CHECK(outc[0] == expect);  // == the CPU oracle (integer-valued -> exact)
}

TEST_CASE("ceir 13z: scan CEIR asset on CUDA == direct CKIR (byte-identical) + oracle", "[ceir][ceir-gpu][cuda][gpu]")
{
    namespace gpu = crd::gpu;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(alloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping"); return; }
    gpu::CudaComputeContext& compute = *cudactx;

    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_scan_block(g, n, n, /*inclusive=*/true, /*write_blocksum=*/true);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_cuda(g, e, &alloc, kern));
    auto pipe = compute.create_pipeline_from_cuda(crd::containers::to_view(kern.source), crd::containers::StringView("ckir"), 3, e.local_size[0], 0U, /*fmad*/ false);
    REQUIRE(pipe != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDispatchAsset asset = cgt::build_ceir_dispatch_asset(cctx, "scan", "r,w,w", 3);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    REQUIRE(cmds.size() == 1U);

    float in0[n];
    float pref[n];
    float total = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        total += in0[i];
        pref[i] = total;
    }
    const int lens[3] = {n, n, 1};

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
    CHECK(bsc[0] == total);                                    // bsum[0] == the span total
}

// ⭐ CEIR-13z-3 (CUDA) — the 6-dispatch 2D FFT as a MULTI-DISPATCH CEIR asset on a real CUDA device. This is the test that
// caught + drove the CUDA block-dim fix (create_pipeline_from_cuda now takes the kernel's local_size). (1) the CEIR path is
// BYTE-IDENTICAL to the direct dispatch_fft2d (same pipelines + CEIR-derived barriers); (2) == the CPU oracle (run_fft2d_cpu).
TEST_CASE("ceir 13z: a 6-dispatch 2D FFT CEIR asset on CUDA == direct dispatch_fft2d + CPU oracle", "[ceir][ceir-gpu][cuda][gpu][fft]")
{
    namespace gpu = crd::gpu;
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(alloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping"); return; }
    gpu::CudaComputeContext& compute = *cudactx;

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
    crd::containers::Array<float>    aref(&alloc);
    crd::containers::Array<float>    acei(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    aref.resize(static_cast<crd::usize>(total), 0.0F);
    acei.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[16];
    float*    href[16];
    float*    hcei[16];
    for (int b = 0; b < plan.nbuffers; ++b)
    {
        h64[b]  = a64.data() + off[b];
        href[b] = aref.data() + off[b];
        hcei[b] = acei.data() + off[b];
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
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc);
        h64[plan.tw_col_re][k] = f32d(crd::math::cos(a));
        h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a));
    }
    for (int k = 0; k < rr; ++k)
    {
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr);
        h64[plan.tw_row_re][k] = f32d(crd::math::cos(a));
        h64[plan.tw_row_im][k] = f32d(-crd::math::sin(a));
    }
    for (int i = 0; i < total; ++i)
    {
        aref[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]);
        acei[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]);
    }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc); // CPU oracle

    std::unique_ptr<gpu::ComputePipeline> pipe_store[8];
    gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_cuda(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        // ⭐ the fix: each pass's pipeline carries ITS OWN local_size (blockDim.x) — the FFT passes are not all 256.
        pipe_store[pi] = compute.create_pipeline_from_cuda(crd::containers::to_view(kern.source), crd::containers::StringView("ckir"),
                                                           plan.passes[pi].nbind, plan.passes[pi].entry.local_size[0], 0U,
                                                           /*fmad*/ false);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(compute, plan, pipes, href); // direct path (byte-identical reference)

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
    crd::memory::MallocAllocator                croot;
    ce::Context                                 cctx(&croot);
    const cgt::CeirMultiAsset                   asset = cgt::build_ceir_multi_asset(cctx, plan.nbuffers, mp, plan.npasses);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    int sizes[16];
    for (int b = 0; b < plan.nbuffers; ++b) { sizes[b] = plan.buffers[b].size; }
    const ceg::ExecuteError err = cgt::dispatch_ceir_multi(
        cctx, asset, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), sizes, pipes, compute, hcei);
    CHECK(err == ceg::ExecuteError::None);

    // ⭐ THE CEIR CORRECTNESS CLAIM: the CEIR path is BYTE-IDENTICAL to the direct dispatch_fft2d (same pipelines + the
    // CEIR-derived barriers) — exact, on CUDA. This is what proves the CEIR lowering + execute_lowered are correct here.
    int bad_vs_direct = 0;
    for (int i = 0; i < total; ++i)
    {
        if (acei[static_cast<crd::usize>(i)] != aref[static_cast<crd::usize>(i)]) { ++bad_vs_direct; }
    }
    CHECK(bad_vs_direct == 0);

    // == the CPU oracle, BIT-EXACT: these pipelines were compiled with `fmad=false` (per-kernel choice — the CKIR kernels are
    // the oracle-matched path), so CUDA's multiply-adds round per-op exactly like `run_fft2d_cpu`. (Before the block-dim fix
    // this was garbage/maxrel=1; with fmad=true it was a few ULPs off — bit-exact confirms both fixes.)
    int badr = 0;
    int badi = 0;
    for (int i = 0; i < rr * cc; ++i)
    {
        if (hcei[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
        if (hcei[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
    }
    CHECK(badr == 0);
    CHECK(badi == 0);
}
