#pragma once

// ckir_abuffer_test.hpp — B17-c: the SHARED both-backend harness for the EXACT-REFERENCE A-buffer OIT (crd::kir::oit).
// The SAME 4-quad translucent scene the WBOIT test (ckir_oit_test.hpp) uses is stored as per-pixel fragments, then SORTED
// by depth and composited EXACTLY (front-to-back over) on Vulkan (GLSL) AND DX12 (HLSL). The composite is pure f32
// mul/add/sub on a deterministic order ⇒ BIT-EXACT vs `eval_cpu_kernel` (the f32-rounded CPU oracle) and Vulkan == DX12.
// This exact composite is the GROUND TRUTH the approximate WBOIT/MBOIT tiers are measured against.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_oit.hpp>

#include <ckir_oit_test.hpp> // crd::gputest::WboitScene (the shared translucent scene, shared with WBOIT)

#include <crd/gpu/compute.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>

namespace crd::kir_test
{

// Flatten a WboitScene into the A-buffer `scene` buffer layout: `layers*5` values = [r,g,b,a,depth] per layer.
inline void abuffer_fill_scene(const crd::gputest::WboitScene& scene, crd::f64* dst)
{
    for (crd::u32 q = 0; q < scene.count; ++q)
    {
        dst[q * 5U + 0U] = static_cast<crd::f64>(scene.color[q][0]);
        dst[q * 5U + 1U] = static_cast<crd::f64>(scene.color[q][1]);
        dst[q * 5U + 2U] = static_cast<crd::f64>(scene.color[q][2]);
        dst[q * 5U + 3U] = static_cast<crd::f64>(scene.alpha[q]);
        dst[q * 5U + 4U] = static_cast<crd::f64>(scene.depth[q]);
    }
}

// The resolve-kernel builder — `build_abuffer_resolve` (exact) or `build_mboit_resolve` (moment-based); both read the SAME
// deferred fragment store and write W*H*3 interleaved RGB.
using ResolveBuilder = crd::kir::KEntry (*)(crd::kir::KGraph&, const crd::kir::oit::AbufferConfig&);

// CPU ORACLE: run the shared build + the chosen resolve kernel through `eval_cpu_kernel` (f64 buffers, every op F32-rounded),
// writing the composited W*H*3 RGB (interleaved) into `out`. The reference for both device backends.
inline void oit_oracle(const crd::kir::oit::AbufferConfig& cfg, const crd::gputest::WboitScene& scene,
                       crd::memory::IAllocator& alloc, ResolveBuilder resolve_build, crd::containers::Array<crd::f64>& out)
{
    crd::kir::KGraph bg(&alloc);
    crd::kir::KEntry be = crd::kir::oit::build_abuffer_store(bg, cfg);
    crd::kir::KGraph rg(&alloc);
    crd::kir::KEntry re = resolve_build(rg, cfg);

    const crd::u32 wh    = cfg.width * cfg.height;
    const crd::u32 total = wh * cfg.layers;

    crd::containers::Array<crd::f64> scene64(&alloc);
    scene64.resize(static_cast<crd::usize>(cfg.layers) * 5U, 0.0);
    abuffer_fill_scene(scene, scene64.data());

    crd::containers::Array<crd::f64> nr(&alloc), ng(&alloc), nb(&alloc), na(&alloc), nd(&alloc);
    nr.resize(total, 0.0); ng.resize(total, 0.0); nb.resize(total, 0.0); na.resize(total, 0.0); nd.resize(total, 0.0);

    crd::kir::KernelBuffer bbufs[6] = {{scene64.data(), static_cast<int>(cfg.layers * 5U), 0, 0},
                                       {nr.data(), static_cast<int>(total), 0, 1},
                                       {ng.data(), static_cast<int>(total), 0, 2},
                                       {nb.data(), static_cast<int>(total), 0, 3},
                                       {na.data(), static_cast<int>(total), 0, 4},
                                       {nd.data(), static_cast<int>(total), 0, 5}};
    const crd::u32 grid_build = (total + cfg.local_size - 1U) / cfg.local_size;
    crd::kir::eval_cpu_kernel(bg, be, bbufs, 6, cfg.local_size, &alloc, grid_build);

    out.resize(static_cast<crd::usize>(wh) * 3U, 0.0);
    crd::kir::KernelBuffer rbufs[6] = {{nr.data(), static_cast<int>(total), 0, 0},
                                       {ng.data(), static_cast<int>(total), 0, 1},
                                       {nb.data(), static_cast<int>(total), 0, 2},
                                       {na.data(), static_cast<int>(total), 0, 3},
                                       {nd.data(), static_cast<int>(total), 0, 4},
                                       {out.data(), static_cast<int>(wh * 3U), 0, 5}};
    const crd::u32 grid_res = (wh + cfg.local_size - 1U) / cfg.local_size;
    crd::kir::eval_cpu_kernel(rg, re, rbufs, 6, cfg.local_size, &alloc, grid_res);
}

// GPU DISPATCH: the shared build (deferred fragment store) then the chosen resolve kernel, chained with barriers.
// `make_pipe(graph, entry, nbufs)` compiles a backend pipeline (GLSL→SPIR-V on Vulkan, HLSL→DXIL on DX12). Reads RGB into `out`.
template <typename MakePipe>
inline void oit_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe, const crd::kir::oit::AbufferConfig& cfg,
                         const crd::gputest::WboitScene& scene, crd::memory::IAllocator& alloc,
                         ResolveBuilder resolve_build, crd::containers::Array<float>& out)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    crd::kir::KGraph bg(&alloc);
    crd::kir::KEntry be = crd::kir::oit::build_abuffer_store(bg, cfg);
    crd::kir::KGraph rg(&alloc);
    crd::kir::KEntry re = resolve_build(rg, cfg);
    auto             build_pipe   = make_pipe(bg, be, 6);
    auto             resolve_pipe = make_pipe(rg, re, 6);

    const crd::u32 wh    = cfg.width * cfg.height;
    const crd::u32 total = wh * cfg.layers;
    const crd::u64 sc_b  = static_cast<crd::u64>(cfg.layers) * 5U * sizeof(float);
    const crd::u64 nb_b  = static_cast<crd::u64>(total) * sizeof(float);
    const crd::u64 out_b = static_cast<crd::u64>(wh) * 3U * sizeof(float);

    auto sc_dev  = ctx.create_buffer(sc_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto nr_dev  = ctx.create_buffer(nb_b, storage, g::ComputeMemory::GpuOnly);
    auto ng_dev  = ctx.create_buffer(nb_b, storage, g::ComputeMemory::GpuOnly);
    auto nb_dev  = ctx.create_buffer(nb_b, storage, g::ComputeMemory::GpuOnly);
    auto na_dev  = ctx.create_buffer(nb_b, storage, g::ComputeMemory::GpuOnly);
    auto nd_dev  = ctx.create_buffer(nb_b, storage, g::ComputeMemory::GpuOnly);
    auto out_dev = ctx.create_buffer(out_b, storage | transfer_src, g::ComputeMemory::GpuOnly);
    auto sc_up   = ctx.create_buffer(sc_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto out_rb  = ctx.create_buffer(out_b, transfer_dst, g::ComputeMemory::GpuToCpu);

    auto* sp = static_cast<float*>(sc_up->map());
    for (crd::u32 q = 0; q < cfg.layers; ++q)
    {
        sp[q * 5U + 0U] = scene.color[q][0];
        sp[q * 5U + 1U] = scene.color[q][1];
        sp[q * 5U + 2U] = scene.color[q][2];
        sp[q * 5U + 3U] = scene.alpha[q];
        sp[q * 5U + 4U] = scene.depth[q];
    }
    sc_up->unmap();

    auto& rec = ctx.begin();
    rec.copy(*sc_up, *sc_dev, 0U, 0U, sc_b);
    rec.barrier(*sc_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    g::ComputeBuffer* bbind[6] = {sc_dev.get(), nr_dev.get(), ng_dev.get(), nb_dev.get(), na_dev.get(), nd_dev.get()};
    const crd::u32    gb       = (total + cfg.local_size - 1U) / cfg.local_size;
    rec.dispatch(*build_pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(bbind, 6), nullptr, 0U, gb, 1U, 1U);
    rec.barrier(*nr_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    rec.barrier(*ng_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    rec.barrier(*nb_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    rec.barrier(*na_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    rec.barrier(*nd_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    g::ComputeBuffer* rbind[6] = {nr_dev.get(), ng_dev.get(), nb_dev.get(), na_dev.get(), nd_dev.get(), out_dev.get()};
    const crd::u32    gr       = (wh + cfg.local_size - 1U) / cfg.local_size;
    rec.dispatch(*resolve_pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(rbind, 6), nullptr, 0U, gr, 1U, 1U);
    rec.barrier(*out_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*out_dev, *out_rb, 0U, 0U, out_b);
    ctx.submit_and_wait();

    out.resize(static_cast<crd::usize>(wh) * 3U, 0.0F);
    const auto* r = static_cast<const float*>(out_rb->map());
    for (crd::u32 i = 0; i < wh * 3U; ++i) { out[static_cast<crd::usize>(i)] = r[i]; }
    out_rb->unmap();
}

// B17-c A-buffer (exact) wrappers.
inline void abuffer_oracle(const crd::kir::oit::AbufferConfig& cfg, const crd::gputest::WboitScene& scene,
                           crd::memory::IAllocator& alloc, crd::containers::Array<crd::f64>& out)
{
    oit_oracle(cfg, scene, alloc, &crd::kir::oit::build_abuffer_resolve, out);
}
template <typename MakePipe>
inline void abuffer_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe, const crd::kir::oit::AbufferConfig& cfg,
                             const crd::gputest::WboitScene& scene, crd::memory::IAllocator& alloc,
                             crd::containers::Array<float>& out)
{
    oit_dispatch(ctx, make_pipe, cfg, scene, alloc, &crd::kir::oit::build_abuffer_resolve, out);
}

// B17-b MBOIT (moment-based) wrappers — same deferred store, moment reconstruction resolve.
inline void mboit_oracle(const crd::kir::oit::AbufferConfig& cfg, const crd::gputest::WboitScene& scene,
                         crd::memory::IAllocator& alloc, crd::containers::Array<crd::f64>& out)
{
    oit_oracle(cfg, scene, alloc, &crd::kir::oit::build_mboit_resolve, out);
}
template <typename MakePipe>
inline void mboit_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe, const crd::kir::oit::AbufferConfig& cfg,
                           const crd::gputest::WboitScene& scene, crd::memory::IAllocator& alloc,
                           crd::containers::Array<float>& out)
{
    oit_dispatch(ctx, make_pipe, cfg, scene, alloc, &crd::kir::oit::build_mboit_resolve, out);
}

// B17-b (extension) 6-power-moment MBOIT wrappers — the hero tier lifted to 3-mass depth complexity.
inline void mboit6_oracle(const crd::kir::oit::AbufferConfig& cfg, const crd::gputest::WboitScene& scene,
                          crd::memory::IAllocator& alloc, crd::containers::Array<crd::f64>& out)
{
    oit_oracle(cfg, scene, alloc, &crd::kir::oit::build_mboit6_resolve, out);
}
template <typename MakePipe>
inline void mboit6_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe, const crd::kir::oit::AbufferConfig& cfg,
                            const crd::gputest::WboitScene& scene, crd::memory::IAllocator& alloc,
                            crd::containers::Array<float>& out)
{
    oit_dispatch(ctx, make_pipe, cfg, scene, alloc, &crd::kir::oit::build_mboit6_resolve, out);
}

// B17-c (scalable) STOCHASTIC TRANSPARENCY wrappers — a SINGLE kernel (reads the scene fragments directly, no deferred store,
// no per-pixel list): S sub-samples, each keeping the nearest hash-covered fragment; the mean is the unbiased `over` estimate.
inline void stochastic_oracle(const crd::kir::oit::AbufferConfig& cfg, const crd::gputest::WboitScene& scene,
                              crd::memory::IAllocator& alloc, crd::containers::Array<crd::f64>& out)
{
    crd::kir::KGraph sg(&alloc);
    crd::kir::KEntry se = crd::kir::oit::build_stochastic_resolve(sg, cfg);
    const crd::u32   wh = cfg.width * cfg.height;

    crd::containers::Array<crd::f64> scene64(&alloc);
    scene64.resize(static_cast<crd::usize>(cfg.layers) * 5U, 0.0);
    abuffer_fill_scene(scene, scene64.data());
    out.resize(static_cast<crd::usize>(wh) * 3U, 0.0);

    crd::kir::KernelBuffer bufs[2] = {{scene64.data(), static_cast<int>(cfg.layers * 5U), 0, 0},
                                      {out.data(), static_cast<int>(wh * 3U), 0, 1}};
    const crd::u32         grid    = (wh + cfg.local_size - 1U) / cfg.local_size;
    crd::kir::eval_cpu_kernel(sg, se, bufs, 2, cfg.local_size, &alloc, grid);
}
template <typename MakePipe>
inline void stochastic_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe, const crd::kir::oit::AbufferConfig& cfg,
                                const crd::gputest::WboitScene& scene, crd::memory::IAllocator& alloc,
                                crd::containers::Array<float>& out)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    crd::kir::KGraph sg(&alloc);
    crd::kir::KEntry se   = crd::kir::oit::build_stochastic_resolve(sg, cfg);
    auto             pipe = make_pipe(sg, se, 2);

    const crd::u32 wh    = cfg.width * cfg.height;
    const crd::u64 sc_b  = static_cast<crd::u64>(cfg.layers) * 5U * sizeof(float);
    const crd::u64 out_b = static_cast<crd::u64>(wh) * 3U * sizeof(float);

    auto sc_dev  = ctx.create_buffer(sc_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto out_dev = ctx.create_buffer(out_b, storage | transfer_src, g::ComputeMemory::GpuOnly);
    auto sc_up   = ctx.create_buffer(sc_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto out_rb  = ctx.create_buffer(out_b, transfer_dst, g::ComputeMemory::GpuToCpu);

    auto* sp = static_cast<float*>(sc_up->map());
    for (crd::u32 q = 0; q < cfg.layers; ++q)
    {
        sp[q * 5U + 0U] = scene.color[q][0]; sp[q * 5U + 1U] = scene.color[q][1]; sp[q * 5U + 2U] = scene.color[q][2];
        sp[q * 5U + 3U] = scene.alpha[q];    sp[q * 5U + 4U] = scene.depth[q];
    }
    sc_up->unmap();

    auto& rec = ctx.begin();
    rec.copy(*sc_up, *sc_dev, 0U, 0U, sc_b);
    rec.barrier(*sc_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    g::ComputeBuffer* bind[2] = {sc_dev.get(), out_dev.get()};
    const crd::u32    gr      = (wh + cfg.local_size - 1U) / cfg.local_size;
    rec.dispatch(*pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(bind, 2), nullptr, 0U, gr, 1U, 1U);
    rec.barrier(*out_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*out_dev, *out_rb, 0U, 0U, out_b);
    ctx.submit_and_wait();

    out.resize(static_cast<crd::usize>(wh) * 3U, 0.0F);
    const auto* r = static_cast<const float*>(out_rb->map());
    for (crd::u32 i = 0; i < wh * 3U; ++i) { out[static_cast<crd::usize>(i)] = r[i]; }
    out_rb->unmap();
}

// B17-c (scalable) ATOMIC LINKED-LIST A-buffer dispatch: the atomic build (dynamic fragment capture into per-pixel lists)
// then the list-walking resolve. Pre-clears the counter (0) + head (EMPTY). Reads the composited RGB into `out`. The list
// order is race-nondeterministic but the resolve sorts ⇒ `out` must match the static-slot exact reference bit-for-bit.
template <typename MakePipe>
inline void abuffer_atomic_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe,
                                    const crd::kir::oit::AbufferConfig& cfg, const crd::gputest::WboitScene& scene,
                                    crd::memory::IAllocator& alloc, crd::containers::Array<float>& out)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    crd::kir::KGraph bg(&alloc);
    crd::kir::KEntry be = crd::kir::oit::build_abuffer_atomic_build(bg, cfg);
    crd::kir::KGraph rg(&alloc);
    crd::kir::KEntry re           = crd::kir::oit::build_abuffer_atomic_resolve(rg, cfg);
    auto             build_pipe   = make_pipe(bg, be, 5);
    auto             resolve_pipe = make_pipe(rg, re, 4);

    const crd::u32 wh    = cfg.width * cfg.height;
    const crd::u32 total = wh * cfg.layers;
    const crd::u64 sc_b  = static_cast<crd::u64>(cfg.layers) * 5U * sizeof(float);
    const crd::u64 cnt_b = sizeof(crd::u32);
    const crd::u64 hd_b  = static_cast<crd::u64>(wh) * sizeof(crd::u32);
    const crd::u64 nx_b  = static_cast<crd::u64>(total) * sizeof(crd::u32);       // next-pointer array
    const crd::u64 node_b = static_cast<crd::u64>(total) * 5U * sizeof(float);    // interleaved node pool (r,g,b,a,depth)
    const crd::u64 out_b = static_cast<crd::u64>(wh) * 3U * sizeof(float);

    auto sc_dev   = ctx.create_buffer(sc_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto cnt_dev  = ctx.create_buffer(cnt_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto hd_dev   = ctx.create_buffer(hd_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto nx_dev   = ctx.create_buffer(nx_b, storage, g::ComputeMemory::GpuOnly);
    auto node_dev = ctx.create_buffer(node_b, storage, g::ComputeMemory::GpuOnly);
    auto out_dev  = ctx.create_buffer(out_b, storage | transfer_src, g::ComputeMemory::GpuOnly);
    auto sc_up    = ctx.create_buffer(sc_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto cnt_up   = ctx.create_buffer(cnt_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto hd_up    = ctx.create_buffer(hd_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto out_rb   = ctx.create_buffer(out_b, transfer_dst, g::ComputeMemory::GpuToCpu);

    auto* sp = static_cast<float*>(sc_up->map());
    for (crd::u32 q = 0; q < cfg.layers; ++q)
    {
        sp[q * 5U + 0U] = scene.color[q][0]; sp[q * 5U + 1U] = scene.color[q][1]; sp[q * 5U + 2U] = scene.color[q][2];
        sp[q * 5U + 3U] = scene.alpha[q];    sp[q * 5U + 4U] = scene.depth[q];
    }
    sc_up->unmap();
    *static_cast<crd::u32*>(cnt_up->map()) = 0U;
    cnt_up->unmap();
    auto* hp = static_cast<crd::u32*>(hd_up->map());
    for (crd::u32 i = 0; i < wh; ++i) { hp[i] = crd::kir::oit::kAbufferEmpty; } // empty lists
    hd_up->unmap();

    auto& rec = ctx.begin();
    rec.copy(*sc_up, *sc_dev, 0U, 0U, sc_b);
    rec.copy(*cnt_up, *cnt_dev, 0U, 0U, cnt_b);
    rec.copy(*hd_up, *hd_dev, 0U, 0U, hd_b);
    rec.barrier(*sc_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*cnt_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderWrite);
    rec.barrier(*hd_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderWrite);
    g::ComputeBuffer* bbind[5] = {sc_dev.get(), cnt_dev.get(), hd_dev.get(), nx_dev.get(), node_dev.get()};
    const crd::u32    gb       = (total + cfg.local_size - 1U) / cfg.local_size;
    rec.dispatch(*build_pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(bbind, 5), nullptr, 0U, gb, 1U, 1U);
    rec.barrier(*hd_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    rec.barrier(*nx_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    rec.barrier(*node_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
    g::ComputeBuffer* rbind[4] = {hd_dev.get(), nx_dev.get(), node_dev.get(), out_dev.get()};
    const crd::u32    gr       = (wh + cfg.local_size - 1U) / cfg.local_size;
    rec.dispatch(*resolve_pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(rbind, 4), nullptr, 0U, gr, 1U, 1U);
    rec.barrier(*out_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*out_dev, *out_rb, 0U, 0U, out_b);
    ctx.submit_and_wait();

    out.resize(static_cast<crd::usize>(wh) * 3U, 0.0F);
    const auto* r = static_cast<const float*>(out_rb->map());
    for (crd::u32 i = 0; i < wh * 3U; ++i) { out[static_cast<crd::usize>(i)] = r[i]; }
    out_rb->unmap();
}

} // namespace crd::kir_test
