// test_dx12_compute.cpp — the D3D12 IComputeContext (ADR-0100) on the GPU. Proves the ONE backend-agnostic dispatch
// surface is genuinely backend-neutral: the SAME crd::gpu::IComputeContext API that drives geometry + CKIR on Vulkan
// runs a real kernel on DirectX 12. Vector-add through the full portable path (upload → dispatch → copy-readback),
// verified vs the CPU. This is the second backend that the seam claims to support — here it is, running.

#include <crd/gpu/dx12_compute_context.hpp>

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
