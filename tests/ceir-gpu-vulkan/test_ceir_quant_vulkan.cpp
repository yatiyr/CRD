// CEIR-23b-1 (Vulkan) — the authored Q8 DEQUANT kernel on a REAL Vulkan device. ckir_read assets/ckir/quant_dequantize_q8.ckir
// → emit GLSL → SPIR-V → dispatch over ONE workgroup (the portable dispatch_kernel_1wg harness), validated vs the CPU reference
// (int8 - zp)*scale. ⛔ the int8 weights are U32-PACKED (4/u32) + SIGN-EXTENDED IN FLOAT in the kernel — this DEVICE leg catches a
// GLSL/HLSL emitter divergence the device-free eval oracle cannot (the dx12-hlsl-masks scar class); the DX12 twin is
// test_ceir_quant_dx12.cpp. ⛔ dispatch_kernel_1wg is float-only, so the packed U32 buffer is uploaded as the BIT-REINTERPRET of
// each word (4 bytes land identically; the kernel's BufferDecl reads them as U32).

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
constexpr int kN = 64; // N % 4 == 0

float bits_as_float(crd::u32 w)
{
    float f = 0.0F;
    std::memcpy(&f, &w, sizeof(f));
    return f;
}
} // namespace

TEST_CASE("ceir 23b-1: quant_dequantize_q8 runs on a Vulkan device (u32-pack + sign-extend) vs the CPU ref", "[ceir][ckir][quant][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    {
        std::ifstream f(CRD_REPO_DIR "/assets/ckir/quant_dequantize_q8.ckir", std::ios::binary | std::ios::ate);
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

    // ── the reference: int8 values (mix of sign), packed 4/u32; out = (int8 - zp)*scale. ──
    crd::i32 b[kN];
    for (int i = 0; i < kN; ++i) { b[i] = ((i * 37 + 11) % 256) - 128; }
    float packed[kN / 4];
    for (int w = 0; w < kN / 4; ++w)
    {
        crd::u32 word = 0;
        for (int j = 0; j < 4; ++j) { word |= static_cast<crd::u32>(b[4 * w + j] & 0xFF) << (8U * static_cast<crd::u32>(j)); }
        packed[w] = bits_as_float(word);
    }
    float scale[1] = {0.5F};
    float zp[1]    = {3.0F};
    float out[kN];
    for (int i = 0; i < kN; ++i) { out[i] = -999.0F; }

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-23b-1 dequant gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const auto spv = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                     "quant_dequant", &root);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 4, 0U);
    REQUIRE(pipe != nullptr);

    float*    host[4] = {packed, scale, zp, out};
    const int lens[4] = {kN / 4, 1, 1, kN};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 4, 1U);

    const auto absf = [](float x) { return x < 0.0F ? -x : x; };
    for (int i = 0; i < kN; ++i)
    {
        const float ref = (static_cast<float>(b[i]) - zp[0]) * scale[0];
        CHECK(absf(out[i] - ref) <= 1e-5F * (1.0F + absf(ref)));
    }
}
