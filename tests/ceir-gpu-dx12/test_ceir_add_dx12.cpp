// CEIR-13z-1b (DX12) — the DirectX 12 mirror of test_ceir_add_vulkan.cpp: the SAME `add` CEIR asset + the SAME
// backend-agnostic harness (`dispatch_ceir_1wg`), now compiling the CKIR add kernel to HLSL (`emit_compute_kernel_hlsl`) and
// running on a real D3D12 device through `execute_lowered` (ADR-0126). Proven BYTE-IDENTICAL to the direct CKIR dispatch of
// the same kernel + == the CPU oracle. ⛔ Windows-only (crd-kir-dx12 is guarded out on Linux); NOT in gate6b.sh.
// ⛔ ValidationCapture-silent is NOT asserted here: gpu-context-dx12 has no info-queue capture mechanism yet (the Vulkan
// backend owns that DoD leg; the DX12 equivalent is a named-forward, exactly as the design note pins it).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_fft.hpp>    // CEIR-13z-3: build_fft2d_c2c
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_reduce.hpp> // CEIR-13z-2: build_reduce_block
#include <crd/kir/ckir_scan.hpp>   // CEIR-13z-2: build_scan_block

#include <crd/math/cmath.hpp> // CEIR-13z-3: cos/sin for the FFT twiddle tables

#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ceir_execute_1wg.hpp"
#include "../gpu-shared/ckir_kernel_dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace cgt = crd::ceir_gpu_test;

TEST_CASE("ceir 13z: the add CEIR asset lowers to one dispatch over 3 bindings (dx12, device-free always-runs)", "[ceir][ceir-gpu][dx12]")
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

TEST_CASE("ceir 13z: add CEIR asset on DX12 == direct CKIR (byte-identical) + oracle", "[ceir][ceir-gpu][dx12][gpu]")
{
    namespace gpu = crd::gpu;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::Dx12ComputeContext    compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device available; skipping"); return; }

    // build + emit + compile the add kernel to a DXIL pipeline (3 storage bindings, no push constant)
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = cgt::build_add_kernel(g, n);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_hlsl(g, e, &alloc, kern));
    auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 3, 0U);
    REQUIRE(pipe != nullptr);

    // build + lower the CEIR asset
    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const ce::Value*             binds[3];
    ce::Block* const             blk = cgt::build_add_ceir_asset(cctx, binds);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *blk, cmds);
    REQUIRE(cmds.size() == 1U);

    // inputs (f32 add is one IEEE op -> bit-exact)
    float a0[n];
    float b0[n];
    for (int i = 0; i < n; ++i)
    {
        a0[i] = static_cast<float>(i) * 0.5F;
        b0[i] = static_cast<float>(n - i) * 0.25F;
    }
    const int lens[3] = {n, n, n};

    // DIRECT path (rec.dispatch) — the reference.
    float ad[n];
    float bd[n];
    float cd[n];
    for (int i = 0; i < n; ++i) { ad[i] = a0[i]; bd[i] = b0[i]; cd[i] = 0.0F; }
    float* hd[3] = {ad, bd, cd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 3, 1U);

    // CEIR path (execute_lowered) — the SAME pipe, separate output.
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

TEST_CASE("ceir 13z: reduce CEIR asset on DX12 == direct CKIR (byte-identical) + oracle", "[ceir][ceir-gpu][dx12][gpu]")
{
    namespace gpu = crd::gpu;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::Dx12ComputeContext    compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device available; skipping"); return; }

    // build_reduce_block: ONE workgroup of n threads sum-reduces its span -> out[0]. 2 buffers (in r, out w).
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_reduce_block(g, n, n, crd::kir::KOp::Add);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_hlsl(g, e, &alloc, kern));
    auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDispatchAsset asset = cgt::build_ceir_dispatch_asset(cctx, "reduce", "r,w", 2);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    REQUIRE(cmds.size() == 1U);

    // integer-valued f32 inputs (sum 2016 < 2^24 -> exact under any association order -> naive oracle is `==`)
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

TEST_CASE("ceir 13z: scan CEIR asset on DX12 == direct CKIR (byte-identical) + oracle", "[ceir][ceir-gpu][dx12][gpu]")
{
    namespace gpu = crd::gpu;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::Dx12ComputeContext    compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device available; skipping"); return; }

    // build_scan_block: ONE workgroup inclusive-scans its span; write_blocksum=TRUE -> bsum[0]=total (all 3 buffers live).
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_scan_block(g, n, n, /*inclusive=*/true, /*write_blocksum=*/true);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_hlsl(g, e, &alloc, kern));
    auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 3, 0U);
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

// ⭐ CEIR-13z-3 part 3 (DX12) — the 6-dispatch 2D FFT as a MULTI-DISPATCH CEIR asset on D3D12 (closes 13z-3, both backends).
// The `dispatch_ceir_multi` harness is backend-agnostic — a drop-in; only the context + emitter differ from the Vulkan test.
// Bit-exact vs the CPU oracle (run_fft2d_cpu). ⛔ ValidationCapture-silent stays Vulkan-only (gpu-context-dx12 has no capture).
TEST_CASE("ceir 13z: a 6-dispatch 2D FFT CEIR asset on DX12 == CPU oracle (multi-dispatch per-resource barriers)", "[ceir][ceir-gpu][dx12][gpu][fft]")
{
    namespace gpu = crd::gpu;
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    gpu::Dx12ComputeContext    compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device available; skipping"); return; }

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
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc); // the CPU oracle

    std::unique_ptr<gpu::ComputePipeline> pipe_store[8];
    gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        pipe_store[pi] = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

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
        cctx, asset, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), sizes, pipes, compute, h32);
    CHECK(err == ceg::ExecuteError::None);

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
