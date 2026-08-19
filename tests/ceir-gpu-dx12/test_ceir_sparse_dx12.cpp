// CEIR-23e (DX12) — the DirectX-12 twin of the authored CSR SpMV device gate. ckir_read assets/ckir/spmv_csr.ckir -> emit HLSL ->
// dispatch over ONE workgroup (dispatch_kernel_1wg) on a REAL D3D12 device, validated vs the CPU CSR SpMV reference
// y[i] = sum_{k in [row_ptr[i], row_ptr[i+1])} values[k]*x[col_idx[k]]. ⛔ THE emitter-agreement check: the runtime For +
// per-thread ForBreakIf + col_idx INDIRECTION must lower IDENTICALLY in HLSL and GLSL (the eval oracle tests the graph, not the
// two emitters). dispatch_kernel_1wg is float-only -> the u32 row_ptr / col_idx buffers upload as the bit-reinterpret of each index.

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
float bits_as_float(crd::u32 w)
{
    float f = 0.0F;
    std::memcpy(&f, &w, sizeof(f));
    return f;
}
} // namespace

TEST_CASE("ceir 23e: spmv_csr runs on a DX12 device (runtime For + ForBreakIf + col_idx) vs the CPU CSR SpMV ref",
          "[ceir][ckir][sparse][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::memory::TlsfAllocator         alloc(16U << 20U);
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
    REQUIRE(kir::emit_compute_kernel_hlsl(kg, ke, &root, kern));

    // a 4x4 CSR matrix, row lengths 2,3,1,2 (nnz=8, max row length 3 == the kernel's baked bound), N=4.
    const crd::u32 rp_u[5] = {0U, 2U, 5U, 6U, 8U};
    const crd::u32 ci_u[8] = {0U, 1U, 0U, 2U, 3U, 1U, 2U, 3U};
    float          rp_f[5] = {};
    float          ci_f[8] = {};
    for (int i = 0; i < 5; ++i) { rp_f[i] = bits_as_float(rp_u[i]); } // u32 row_ptr uploaded as bit-reinterpret
    for (int i = 0; i < 8; ++i) { ci_f[i] = bits_as_float(ci_u[i]); } // u32 col_idx uploaded as bit-reinterpret
    float values[8] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    float x[4]      = {2.0F, 3.0F, 5.0F, 7.0F};
    float y[4]      = {-1.0F, -1.0F, -1.0F, -1.0F};

    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-23e SpMV gate"); return; }
    auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 5, 0U);
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
