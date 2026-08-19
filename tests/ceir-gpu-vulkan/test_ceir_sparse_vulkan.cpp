// CEIR-23e (Vulkan) — the authored CSR SpMV kernel on a REAL Vulkan device. ckir_read assets/ckir/spmv_csr.ckir -> emit GLSL ->
// SPIR-V -> dispatch over ONE workgroup (the portable dispatch_kernel_1wg harness), validated vs the CPU CSR SpMV reference
// y[i] = sum_{k in [row_ptr[i], row_ptr[i+1])} values[k]*x[col_idx[k]]. ⛔ this DEVICE leg proves the RUNTIME For + per-thread
// ForBreakIf + the col_idx INDIRECTION compile + run on real hardware (a GLSL/HLSL loop-lowering divergence the device-free eval
// oracle cannot catch; the DX12 twin is test_ceir_sparse_dx12.cpp). ⛔ dispatch_kernel_1wg is float-only, so the u32 row_ptr /
// col_idx buffers are uploaded as the BIT-REINTERPRET of each index (4 bytes land identically; the kernel reads them as U32).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_read
#include <crd/kir/ckir_glsl.hpp>  // emit_compute_kernel_glsl

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include "../gpu-shared/ckir_kernel_dispatch.hpp" // dispatch_kernel_1wg

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcpy (the u32->float bit reinterpret)
#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace kir = crd::kir;

namespace
{
float bits_as_float(crd::u32 w)
{
    float f = 0.0F;
    std::memcpy(&f, &w, sizeof(f));
    return f;
}
} // namespace

TEST_CASE("ceir 23e: spmv_csr runs on a Vulkan device (runtime For + ForBreakIf + col_idx) vs the CPU CSR SpMV ref",
          "[ceir][ckir][sparse][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    {
        std::ifstream f(CRD_REPO_DIR "/assets/ckir/spmv_csr.ckir", std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        f.seekg(0);
        crd::containers::Array<char> src(&root);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
    }
    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern));

    // a 4x4 CSR matrix, row lengths 2,3,1,2 (nnz=8, max row length 3 == the kernel's baked bound), N=4.
    const crd::u32 rp_u[5]  = {0U, 2U, 5U, 6U, 8U};
    const crd::u32 ci_u[8]  = {0U, 1U, 0U, 2U, 3U, 1U, 2U, 3U};
    float          rp_f[5]  = {};
    float          ci_f[8]  = {};
    for (int i = 0; i < 5; ++i) { rp_f[i] = bits_as_float(rp_u[i]); } // u32 row_ptr uploaded as bit-reinterpret
    for (int i = 0; i < 8; ++i) { ci_f[i] = bits_as_float(ci_u[i]); } // u32 col_idx uploaded as bit-reinterpret
    float values[8] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    float x[4]      = {2.0F, 3.0F, 5.0F, 7.0F};
    float y[4]      = {-1.0F, -1.0F, -1.0F, -1.0F};

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-23e SpMV gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const auto spv = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                     "spmv_csr", &root);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U);
    REQUIRE(pipe != nullptr);

    float*    host[5] = {rp_f, ci_f, values, x, y};
    const int lens[5] = {5, 8, 8, 4, 4};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 5, 1U);

    const auto absf = [](float v) { return v < 0.0F ? -v : v; };
    for (int i = 0; i < 4; ++i)
    {
        float acc = 0.0F;
        for (crd::u32 k = rp_u[i]; k < rp_u[i + 1]; ++k) { acc += values[k] * x[ci_u[k]]; }
        CHECK(absf(y[i] - acc) <= 1e-5F * (1.0F + absf(acc)));
    }
}
