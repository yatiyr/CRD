#pragma once

// ckir_kernel_dispatch.hpp — B-cmp Phase 0: the SHARED both-backend harness for the imperative compute-kernel IR
// (shared memory + barriers). A hand-authored kernel graph runs on Vulkan (GLSL→SPIR-V) AND DX12 (HLSL→DXIL) through the
// SAME portable dispatch surface (`crd::gpu::IComputeContext` / `ComputeRecorder`) and is compared to `eval_cpu_kernel`
// bit-for-bit — the "green on both GPUs" gate that proves the emitters agree with the CPU oracle and with each other.
//
// PORTABLE readback (the DX12-safe contract, per dx12_compute_context.hpp): the kernel reads+writes GpuOnly device
// buffers; the host uploads via a CpuToGpu staging buffer (copy → device) and reads back via a GpuToCpu staging buffer
// (device → copy). "Shader writes a host-visible buffer, then map()" is Vulkan-only and does NOT port, so it is avoided.
//
// F32 buffers (not F64): a kernel with F64 storage needs shaderFloat64, which not every device advertises. The Phase-0
// primitives that gate this harness (reverse, transpose, reduction) move/compare values, so F32 is bit-exact everywhere.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_fft.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/gpu/compute.hpp>

#include <crd/containers/span.hpp>

#include <memory>

namespace crd::kir_test
{

// The shared-memory REVERSE kernel over F32 buffers: `shared[lid] = in[lid]; barrier; out[lid] = shared[ls-1-lid]`. The
// cross-thread read is correct ONLY because of the barrier — so this one kernel exercises shared arrays + a workgroup
// barrier + storage R/W, the whole Phase-0 substrate. Buffers: input at (set 0, binding 0), output at (set 0, binding 1).
inline crd::kir::KEntry build_reverse_kernel(crd::kir::KGraph& g, int ls)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});

    const int inbuf  = g.buffer_decl(k::DType::F32, 0, 0, false); // readonly input  at (0,0)
    const int outbuf = g.buffer_decl(k::DType::F32, 0, 1, true);  // writable output at (0,1)
    const int smem   = g.shared_decl(k::DType::F32, ls);
    const int lid    = g.builtin(k::KBuiltin::LocalInvocationIndex);

    const int mark = g.kernel_stmt_mark();
    g.stmt_shared_store(smem, lid, g.buffer_load(inbuf, lid));
    g.stmt_barrier();
    const int revidx = g.binary(k::KOp::Sub, g.constant(static_cast<crd::f64>(ls - 1), sh1, k::DType::U32), lid);
    g.stmt_buffer_store(outbuf, lid, g.shared_load(smem, revidx));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(ls);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// A shared-memory T×T TRANSPOSE (For loops + barrier + cross-thread read) over F32 buffers — the FFT's transpose primitive,
// exercising the structured-control-flow emitters (For body skip) on real hardware. Thread r owns row r: loads row r into
// shared, barrier, writes row r of out = column r of shared. Buffers: input (0,0), output (0,1); ls = T threads.
inline crd::kir::KEntry build_transpose_kernel(crd::kir::KGraph& g, int t)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, k::DType::U32); };

    const int inbuf  = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int outbuf = g.buffer_decl(k::DType::F32, 0, 1, true);
    const int smem   = g.shared_decl(k::DType::F32, t * t);
    const int lid    = g.builtin(k::KBuiltin::LocalInvocationIndex); // = row r

    const int mark = g.kernel_stmt_mark();
    const int f1   = g.stmt_for_begin(ku(static_cast<crd::u32>(t))); // load row r
    const int c1   = g.kernel_loop_var(f1);
    const int ldi  = g.binary(k::KOp::Add, g.binary(k::KOp::Mul, lid, ku(static_cast<crd::u32>(t))), c1);
    g.stmt_shared_store(smem, ldi, g.buffer_load(inbuf, ldi));
    g.stmt_for_end(f1);
    g.stmt_barrier();
    const int f2  = g.stmt_for_begin(ku(static_cast<crd::u32>(t))); // write transposed row
    const int c2  = g.kernel_loop_var(f2);
    const int oid = g.binary(k::KOp::Add, g.binary(k::KOp::Mul, lid, ku(static_cast<crd::u32>(t))), c2);
    const int sid = g.binary(k::KOp::Add, g.binary(k::KOp::Mul, c2, ku(static_cast<crd::u32>(t))), lid); // column r → cross-thread
    g.stmt_buffer_store(outbuf, oid, g.shared_load(smem, sid));
    g.stmt_for_end(f2);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(t);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Dispatch `pipe` over ONE workgroup on the portable surface. `host[b]` is `nbufs` f32 arrays of length `lens[b]`, uploaded
// then overwritten in place with the GPU result. Buffers bind at 0..nbufs-1 (binding order). No push constants (the kernel
// bakes local_size into numthreads/local_size_x), so the pipeline is created with push_size 0.
inline void dispatch_kernel_1wg(crd::gpu::IComputeContext& ctx, crd::gpu::ComputePipeline& pipe, float** host,
                                const int* lens, int nbufs, crd::u32 gx)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    constexpr int                     max_bufs = 8;
    std::unique_ptr<g::ComputeBuffer> dev[max_bufs];
    std::unique_ptr<g::ComputeBuffer> up[max_bufs];
    std::unique_ptr<g::ComputeBuffer> rb[max_bufs];
    g::ComputeBuffer*                 binds[max_bufs] = {};

    for (int b = 0; b < nbufs; ++b)
    {
        const crd::u64 bytes = static_cast<crd::u64>(lens[b]) * sizeof(float);
        dev[b] = ctx.create_buffer(bytes, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly); // R/W, both ways
        up[b]  = ctx.create_buffer(bytes, transfer_src, g::ComputeMemory::CpuToGpu);
        rb[b]  = ctx.create_buffer(bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        auto* p = static_cast<float*>(up[b]->map());
        for (int i = 0; i < lens[b]; ++i) { p[i] = host[b][i]; }
        up[b]->unmap();
        binds[b] = dev[b].get();
    }

    auto& rec = ctx.begin();
    for (int b = 0; b < nbufs; ++b) { rec.copy(*up[b], *dev[b], 0U, 0U, static_cast<crd::u64>(lens[b]) * sizeof(float)); }
    // the upload copies (TransferDst) MUST be visible to the shader's reads — without this barrier a fast kernel (no shared/
    // barrier, e.g. a scan add-offset map) can start before its inputs land and read zeros. Slower shared-mem kernels only
    // masked the race by luck.
    for (int b = 0; b < nbufs; ++b) { rec.barrier(*dev[b], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); }
    rec.dispatch(pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(binds, static_cast<crd::usize>(nbufs)), nullptr, 0U, gx,
                 1U, 1U);
    for (int b = 0; b < nbufs; ++b) // every bound buffer is a UAV → ShaderWrite→TransferSrc is a valid transition for all
    {
        rec.barrier(*dev[b], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*dev[b], *rb[b], 0U, 0U, static_cast<crd::u64>(lens[b]) * sizeof(float));
    }
    ctx.submit_and_wait();

    for (int b = 0; b < nbufs; ++b)
    {
        const auto* r = static_cast<const float*>(rb[b]->map());
        for (int i = 0; i < lens[b]; ++i) { host[b][i] = r[i]; }
        rb[b]->unmap();
    }
}

// ── B-cmp Phase 2: the multi-pass 2-D FFT drivers (a `crd::kir::Fft2dPlan` = an ordered list of dispatches). ────────────

// CPU ORACLE driver: run every pass of `plan` in order over the host f64 buffers `host[b]` (length plan.buffers[b].size),
// binding each pass's logical buffers to its entry's buffer_decls (binding k ← bind[k]). Each pass evaluates against its OWN
// graph (`p.graph`). Buffers persist across passes — exactly what the GPU does. This is the reference the GPU dispatch is
// compared against, bit-for-bit.
inline void run_fft2d_cpu(const crd::kir::Fft2dPlan& plan, crd::f64** host, crd::memory::IAllocator* scratch)
{
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const crd::kir::Fft2dPass& p = plan.passes[pi];
        crd::kir::KernelBuffer     kb[8];
        for (int k = 0; k < p.nbind; ++k)
        {
            kb[k] = crd::kir::KernelBuffer{host[p.bind[k]], plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)};
        }
        crd::kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], scratch, p.num_workgroups);
    }
}

// GPU driver: create ONE GpuOnly device buffer per logical buffer, upload host[b], dispatch each pass (its pipeline
// `pipes[pi]`, the pass's logical buffers bound in order, grid = pass.num_workgroups) with a barrier between passes so the
// next pass reads the previous pass's writes, then read every buffer back into host[b]. Mirrors `run_fft2d_cpu` exactly so
// the GPU float result is bit-comparable to the CPU oracle. `host[b]` / `pipes` are indexed by logical id / pass id.
inline void dispatch_fft2d(crd::gpu::IComputeContext& ctx, const crd::kir::Fft2dPlan& plan,
                           crd::gpu::ComputePipeline** pipes, float** host)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    constexpr int                     max_bufs = 20;
    std::unique_ptr<g::ComputeBuffer> dev[max_bufs];
    std::unique_ptr<g::ComputeBuffer> up[max_bufs];
    std::unique_ptr<g::ComputeBuffer> rb[max_bufs];
    const int                         nb = plan.nbuffers;

    for (int b = 0; b < nb; ++b)
    {
        const int      len   = plan.buffers[b].size;
        const crd::u64 bytes = static_cast<crd::u64>(len) * sizeof(float);
        dev[b] = ctx.create_buffer(bytes, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        up[b]  = ctx.create_buffer(bytes, transfer_src, g::ComputeMemory::CpuToGpu);
        rb[b]  = ctx.create_buffer(bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        auto* p = static_cast<float*>(up[b]->map());
        for (int i = 0; i < len; ++i) { p[i] = host[b][i]; }
        up[b]->unmap();
    }

    auto& rec = ctx.begin();
    for (int b = 0; b < nb; ++b) { rec.copy(*up[b], *dev[b], 0U, 0U, static_cast<crd::u64>(plan.buffers[b].size) * sizeof(float)); }
    // the upload copies (TransferDst) MUST be visible to pass 0's shader reads — without this barrier a large-enough dispatch
    // (grid > device occupancy) races the still-in-flight upload and reads STALE data (flaky, batch-dependent). Same scar as
    // dispatch_kernel_1wg. Latent until B16-a-3's batch=4C (>8 images) exposed it; small-batch multi-pass tests never raced.
    for (int b = 0; b < nb; ++b) { rec.barrier(*dev[b], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); }
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const crd::kir::Fft2dPass& p          = plan.passes[pi];
        g::ComputeBuffer*          binds[8]   = {};
        for (int k = 0; k < p.nbind; ++k) { binds[k] = dev[p.bind[k]].get(); }
        rec.dispatch(*pipes[pi], crd::containers::ConstSpan<g::ComputeBuffer*>(binds, static_cast<crd::usize>(p.nbind)),
                     nullptr, 0U, p.num_workgroups, 1U, 1U);
        if (pi + 1 < plan.npasses) // this pass's UAV writes must be visible to the next pass's reads
        {
            for (int b = 0; b < nb; ++b) { rec.barrier(*dev[b], g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead); }
        }
    }
    for (int b = 0; b < nb; ++b)
    {
        rec.barrier(*dev[b], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*dev[b], *rb[b], 0U, 0U, static_cast<crd::u64>(plan.buffers[b].size) * sizeof(float));
    }
    ctx.submit_and_wait();

    for (int b = 0; b < nb; ++b)
    {
        const auto* r = static_cast<const float*>(rb[b]->map());
        for (int i = 0; i < plan.buffers[b].size; ++i) { host[b][i] = r[i]; }
        rb[b]->unmap();
    }
}

} // namespace crd::kir_test
