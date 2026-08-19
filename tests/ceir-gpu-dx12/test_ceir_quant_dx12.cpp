// CEIR-23b-1 (DX12) — the DirectX-12 twin of the authored Q8 DEQUANT device gate. ckir_read quant_dequantize_q8.ckir → emit
// HLSL → dispatch over ONE workgroup (dispatch_kernel_1wg) on a REAL D3D12 device, validated vs the CPU (int8 - zp)*scale ref.
// ⛔ this is THE emitter-agreement check: the u32-pack + FLOAT sign-extend must emit IDENTICALLY in HLSL and GLSL (the
// dx12-hlsl-masks scar); the eval oracle tests the graph, not the two emitters. dispatch_kernel_1wg is float-only ⇒ the packed
// U32 buffer is uploaded as the bit-reinterpret of each word.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_read
#include <crd/kir/ckir_hlsl.hpp>  // emit_compute_kernel_hlsl

#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ckir_kernel_dispatch.hpp" // dispatch_kernel_1wg

#include <catch2/catch_test_macros.hpp>

#include <cstring>
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

TEST_CASE("ceir 23b-1: quant_dequantize_q8 runs on a DX12 device (u32-pack + sign-extend) vs the CPU ref", "[ceir][ckir][quant][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::memory::TlsfAllocator         alloc(16U << 20U);
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
    REQUIRE(kir::emit_compute_kernel_hlsl(kg, ke, &root, kern));

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

    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-23b-1 dequant gate"); return; }
    auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 4, 0U);
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
