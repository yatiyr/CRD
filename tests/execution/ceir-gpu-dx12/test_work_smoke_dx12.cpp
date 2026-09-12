// CEIR-20b STEP 4 (DX12) — the DirectX 12 mirror of test_work_smoke_vulkan.cpp: the DEVICE PROOF of
// `execute_work_lowered`. An AUTHORED ceir.work program (queue_alloc → produce → consume) runs on a real D3D12 device
// through the WorkHooks: produce (a DIRECT dispatch of work_smoke_produce.ckir) writes the queue's (count=5,1,1) DEVICE
// header; consume (an INDIRECT ExecuteIndirect of work_smoke_consume.ckir) is SIZED by that device-written count, so
// out[0]==5 proves the grid came from the DEVICE, never host. ⛔ Windows-only (crd-kir-dx12 guarded out on Linux);
// soft-skips with no adapter. SINGLE-submit + REPLAYED barriers (the device-resident model): the DX12 IndirectRead
// barrier maps to D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT (dx12_compute:36, C5).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/gpu/work_build.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_read
#include <crd/kir/ckir_hlsl.hpp>  // emit_compute_kernel_hlsl
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace ce = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace gpu = crd::gpu;

namespace
{
// Load a .ckir kernel → compile to a DXIL ComputePipeline (`nbind` storage bindings, no push). Also VALIDATES the
// authored .ckir format (ckir_read → emit HLSL → the DXIL compile inside create_pipeline_from_hlsl).
std::unique_ptr<gpu::ComputePipeline> load_kernel_pipe(const char* path, int nbind, gpu::Dx12ComputeContext& compute,
                                                       crd::memory::IAllocator* alloc)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);
    crd::containers::Array<char> src(alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    crd::kir::KGraph kg(alloc);
    crd::kir::KEntry ke;
    REQUIRE(crd::kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
    crd::kir::GlslKernel kern(alloc); // the emitter fills the same struct; `.source` holds HLSL here
    REQUIRE(crd::kir::emit_compute_kernel_hlsl(kg, ke, alloc, kern));
    auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbind, 0U);
    REQUIRE(pipe != nullptr);
    return pipe;
}

// The WorkHooks context: the caller-owned recorder + the 2 pipelines + the cctx (to read a work op's `kernel` symbol).
struct SmokeHookCtx
{
    gpu::ComputeRecorder* rec = nullptr;
    const ce::Context* cctx = nullptr;
    gpu::ComputePipeline* pipe_produce = nullptr;
    gpu::ComputePipeline* pipe_consume = nullptr;
    crd::u8 sentinel = 1U;
};

crd::containers::ConstSpan<crd::u8> smoke_kernel_bytes(const ce::Operation* /*op*/, void* user)
{
    auto* const c = static_cast<SmokeHookCtx*>(user);
    return crd::containers::ConstSpan<crd::u8>(&c->sentinel, 1U);
}
gpu::ComputePipeline* resolve_pipe(const ce::Operation* op, SmokeHookCtx* c)
{
    const ce::AttrValue kv = c->cctx->attr_value(op->attr(crd::containers::StringView("kernel")));
    if (kv.kind == ce::AttrKind::SymbolRef && kv.s == crd::containers::StringView("work_smoke_produce"))
    {
        return c->pipe_produce;
    }
    return c->pipe_consume;
}
void bind_handles(gpu::ComputeBuffer** binds, crd::containers::ConstSpan<ceg::WorkBufferHandle> handles)
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(handles.size()); ++i)
    {
        binds[i] = reinterpret_cast<gpu::ComputeBuffer*>(handles[i]); // NOLINT(performance-no-int-to-ptr)
    }
}
bool smoke_dispatch(const ce::Operation* op, crd::containers::ConstSpan<crd::u8> /*kb*/,
                    crd::containers::ConstSpan<ceg::WorkBufferHandle> handles, crd::u32 gx, crd::u32 gy, crd::u32 gz,
                    void* user)
{
    auto* const c = static_cast<SmokeHookCtx*>(user);
    gpu::ComputeBuffer* binds[8] = {};
    bind_handles(binds, handles);
    c->rec->dispatch(*resolve_pipe(op, c), crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, handles.size()),
                     nullptr, 0U, gx, gy, gz);
    return true;
}
bool smoke_dispatch_indirect(const ce::Operation* op, ceg::WorkBufferHandle queue,
                             crd::containers::ConstSpan<crd::u8> /*kb*/,
                             crd::containers::ConstSpan<ceg::WorkBufferHandle> handles, void* user)
{
    auto* const c = static_cast<SmokeHookCtx*>(user);
    gpu::ComputeBuffer* binds[8] = {};
    bind_handles(binds, handles);
    auto* const qbuf = reinterpret_cast<gpu::ComputeBuffer*>(queue); // NOLINT(performance-no-int-to-ptr)
    c->rec->dispatch_indirect(*resolve_pipe(op, c),
                              crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, handles.size()), nullptr, 0U,
                              *qbuf, 0U);
    return true;
}
bool smoke_barrier(ceg::WorkBufferHandle buffer, gpu::ComputeAccess from, gpu::ComputeAccess to, void* user)
{
    auto* const c = static_cast<SmokeHookCtx*>(user);
    c->rec->barrier(*reinterpret_cast<gpu::ComputeBuffer*>(buffer), from, to); // NOLINT(performance-no-int-to-ptr)
    return true;
}
} // namespace

TEST_CASE(
    "ceir 20b: the ceir.work executor runs on DX12 -- produce writes the device count, an INDIRECT consume reads it "
    "(device-generated work)",
    "[ceir][ceir-gpu][dx12][gpu][work][ceir20b]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid())
    {
        WARN("no D3D12 device available; skipping");
        return;
    }

    // (a) compile the 2 AUTHORED .ckir smoke kernels → DXIL pipelines (validates the .ckir format).
    auto pipe_produce = load_kernel_pipe(CRD_REPO_DIR "/assets/ckir/work_smoke_produce.ckir", 1, compute, &alloc);
    auto pipe_consume = load_kernel_pipe(CRD_REPO_DIR "/assets/ckir/work_smoke_consume.ckir", 2, compute, &alloc);

    // (b) buffers (the portable dev/up/rb mold).
    constexpr crd::u32 kN = 5U;
    constexpr crd::u64 q_bytes = (3U + 16U) * 4U;
    constexpr crd::u64 o_bytes = 2U * 4U;
    using gpu::compute_usage::indirect;
    using gpu::compute_usage::storage;
    using gpu::compute_usage::transfer_dst;
    using gpu::compute_usage::transfer_src;
    auto q_dev =
        compute.create_buffer(q_bytes, storage | indirect | transfer_dst | transfer_src, gpu::ComputeMemory::GpuOnly);
    auto o_dev = compute.create_buffer(o_bytes, storage | transfer_dst | transfer_src, gpu::ComputeMemory::GpuOnly);
    auto q_up = compute.create_buffer(q_bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
    auto o_up = compute.create_buffer(o_bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
    auto q_rb = compute.create_buffer(q_bytes, transfer_dst, gpu::ComputeMemory::GpuToCpu);
    auto o_rb = compute.create_buffer(o_bytes, transfer_dst, gpu::ComputeMemory::GpuToCpu);
    REQUIRE(q_dev != nullptr);
    REQUIRE(o_dev != nullptr);
    {
        auto* const qp = static_cast<crd::u32*>(q_up->map());
        for (crd::u64 i = 0; i < q_bytes / 4U; ++i)
        {
            qp[i] = 0U;
        }
        q_up->unmap();
        auto* const op = static_cast<crd::u32*>(o_up->map());
        op[0] = 0U;
        op[1] = 0U;
        o_up->unmap();
    }

    // (c) the AUTHORED ceir.work program.
    crd::memory::GrowableTlsfAllocator croot;
    ce::Context cctx(&croot);
    ceg::WorkBuildDesc d;
    d.num_queues = 1U;
    d.queues[0].capacity = 16U;
    d.queues[0].record_stride = 4U;
    d.queues[0].source_param = 0x10U;
    d.num_stages = 2U;
    d.stages[0].kind = ceg::WorkStageKind::Produce;
    d.stages[0].kernel = crd::containers::StringView("work_smoke_produce");
    d.stages[0].queue = 0U;
    d.stages[0].num_bindings = 0U;
    d.stages[1].kind = ceg::WorkStageKind::Consume;
    d.stages[1].kernel = crd::containers::StringView("work_smoke_consume");
    d.stages[1].queue = 0U;
    d.stages[1].num_bindings = 1U;
    d.stages[1].bindings[0].source_param = 0x20U;
    d.stages[1].bindings[0].access = ceg::WorkAccess::ReadWrite;

    crd::containers::Array<ceg::LoweredCommand> plan(&croot);
    ceg::WorkAssetResources res;
    ce::Module* const m = ceg::build_work_ceir(cctx, d, plan, res);
    REQUIRE(m != nullptr);

    crd::containers::Array<ceg::WorkResolvedBinding> table(&croot);
    for (crd::u32 e = 0; e < res.count; ++e)
    {
        gpu::ComputeBuffer* const buf = (res.entries[e].source_param == 0x10U) ? q_dev.get() : o_dev.get();
        table.push_back(ceg::WorkResolvedBinding{res.entries[e].value,
                                                 reinterpret_cast<crd::u64>(buf)}); // NOLINT(performance-no-int-to-ptr)
    }

    // (d) hooks + (e) DRIVE — one submit, caller owns begin/submit + the boundary transfers.
    SmokeHookCtx hctx;
    gpu::ComputeRecorder& rec = compute.begin();
    hctx.rec = &rec;
    hctx.cctx = &cctx;
    hctx.pipe_produce = pipe_produce.get();
    hctx.pipe_consume = pipe_consume.get();
    ceg::WorkHooks hooks;
    hooks.kernel_bytes = &smoke_kernel_bytes;
    hooks.dispatch = &smoke_dispatch;
    hooks.dispatch_indirect = &smoke_dispatch_indirect;
    hooks.barrier = &smoke_barrier;
    hooks.user = &hctx;

    rec.copy(*q_up, *q_dev, 0U, 0U, q_bytes);
    rec.copy(*o_up, *o_dev, 0U, 0U, o_bytes);
    rec.barrier(*q_dev, gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderWrite);
    rec.barrier(*o_dev, gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderWrite);

    const ceg::ExecuteError err = ceg::execute_work_lowered(
        cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(plan.data(), plan.size()), hooks,
        crd::containers::ConstSpan<ceg::WorkResolvedBinding>(table.data(), table.size()));
    CHECK(err == ceg::ExecuteError::None);

    rec.barrier(*q_dev, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::TransferSrc);
    rec.barrier(*o_dev, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::TransferSrc);
    rec.copy(*q_dev, *q_rb, 0U, 0U, q_bytes);
    rec.copy(*o_dev, *o_rb, 0U, 0U, o_bytes);
    compute.submit_and_wait();

    // ⭐ the DEVICE PROOF (distinct non-overwritten slots — single-submit end-of-run readback == per-stage).
    auto* const qout = static_cast<crd::u32*>(q_rb->map());
    auto* const oout = static_cast<crd::u32*>(o_rb->map());
    CHECK(qout[0] == kN); // produce wrote the DEVICE count into the queue header
    CHECK(oout[0] == kN); // ⭐ consume ran N invocations — the ExecuteIndirect was DEVICE-COUNT-sized, not host-sized
    CHECK(oout[1] == kN); // each invocation read queue[0] == the device count
    q_rb->unmap();
    o_rb->unmap();
}
