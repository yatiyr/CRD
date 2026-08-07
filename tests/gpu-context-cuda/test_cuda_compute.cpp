// test_cuda_compute.cpp — the CUDA IComputeContext (ADR-0100, user-directed 2026-08-07) on a real GPU. Proves the ONE
// backend-agnostic dispatch surface (crd::gpu::IComputeContext) runs a real kernel on CUDA: vec-add through the portable
// path (NVRTC->CUBIN pipeline + pinned/UVA buffers + dispatch), == a CPU reference, with GPU event timing. Capability-
// gated: a clean skip (WARN) when there is no CUDA device, so non-NVIDIA runs are unaffected.

#include <crd/gpu/compute.hpp>
#include <crd/gpu/cuda_compute_context.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace g = crd::gpu;

namespace
{
// A vec-add over the portable surface: 3 buffer params + a u32 count push. Grid-strided guard so any (grid × 256) fits.
constexpr const char* kVecAdd = R"cuda(
extern "C" __global__ void vecadd(const float* a, const float* b, float* out, unsigned n)
{
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) { out[i] = a[i] + b[i]; }
}
)cuda";
} // namespace

TEST_CASE("CUDA compute: vec-add through IComputeContext == CPU reference + last_gpu_ms > 0", "[cuda][compute][gpu]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U); // named allocator (no hidden default malloc)
    auto                       ctx = g::create_cuda_compute_context(alloc);
    REQUIRE(ctx != nullptr);
    if (!ctx->valid())
    {
        WARN("no CUDA device available; skipping (capability-gated)");
        return;
    }

    using g::compute_usage::storage;
    constexpr crd::u32 n_elems    = 1024U;
    constexpr crd::u64 bytes = static_cast<crd::u64>(n_elems) * sizeof(float);

    auto ba = ctx->create_buffer(bytes, storage, g::ComputeMemory::CpuToGpu);
    auto bb = ctx->create_buffer(bytes, storage, g::ComputeMemory::CpuToGpu);
    auto bo = ctx->create_buffer(bytes, storage, g::ComputeMemory::GpuToCpu);
    REQUIRE(ba != nullptr);
    REQUIRE(bb != nullptr);
    REQUIRE(bo != nullptr);

    auto* pa = static_cast<float*>(ba->map());
    auto* pb = static_cast<float*>(bb->map());
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    for (crd::u32 i = 0; i < n_elems; ++i)
    {
        pa[i] = static_cast<float>(i);
        pb[i] = static_cast<float>(2U * i);
    }
    ba->unmap();
    bb->unmap();

    auto pipe = ctx->create_pipeline_from_cuda(crd::containers::StringView(kVecAdd), crd::containers::StringView("vecadd"),
                                               /*n_bindings*/ 3, /*push_size*/ sizeof(crd::u32));
    REQUIRE(pipe != nullptr);

    g::ComputeBuffer* binds[3] = {ba.get(), bb.get(), bo.get()};
    crd::u32          count    = n_elems;
    const crd::u32    grid     = (n_elems + 255U) / 256U;

    auto& rec = ctx->begin();
    rec.dispatch(*pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(binds, 3), &count, sizeof(count), grid, 1U, 1U);
    ctx->submit_and_wait();

    auto* po = static_cast<float*>(bo->map());
    REQUIRE(po != nullptr);
    bool ok = true;
    for (crd::u32 i = 0; i < n_elems; ++i)
    {
        if (po[i] != static_cast<float>(3U * i)) { ok = false; break; }
    }
    CHECK(ok);                          // vec-add == CPU reference (a[i]+b[i] == 3i)
    CHECK(ctx->last_gpu_ms() > 0.0);    // real CUDA-event GPU timing
    CHECK(ctx->subgroup_size() == 32U); // NVIDIA warp
}
