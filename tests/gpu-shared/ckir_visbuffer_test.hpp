#pragma once

// ckir_visbuffer_test.hpp — B4-vis: the SHARED both-backend harness for the compute SOFTWARE RASTERIZER (the Nanite
// visibility buffer). A fixed 3-triangle scene (a quad at depth 0.6 + a nearer centre triangle at depth 0.2) is rasterized
// by `build_sw_raster_visbuffer` on Vulkan (GLSL→SPIR-V) AND DX12 (HLSL→DXIL), and the per-pixel visibility key is compared
// to `eval_cpu_kernel` BIT-FOR-BIT. The centre triangle overlaps the quad and is nearer, so the atomicMin depth resolution
// is exercised (centre pixels resolve to the near triangle). Mixed-dtype transfer: positions are f32, indices + the vis
// buffer are u32 — uploaded/read back per element type (the f32-only `dispatch_kernel_1wg` cannot carry the u32 keys).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_visbuffer.hpp>

#include <crd/gpu/compute.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>

#include <memory>

namespace crd::kir_test
{

// The fixed software-rasterizer scene: 7 vertices (clip x,y,z,w), 3 triangles. Triangles 0+1 tile a quad at NDC depth 0.6
// (ids 0,1); triangle 2 is a smaller CCW triangle at the nearer depth 0.2 (id 2) overlapping the quad centre. 32x32 target.
struct SwRasterScene
{
    static constexpr int n_vert = 7;
    static constexpr int n_tri  = 3;

    crd::kir::visbuffer::SwRasterConfig cfg{};
    float                               pos[n_vert * 4] = {};
    crd::u32                            idx[n_tri * 3]  = {};
};

// Author the scene. Quad corners at +-0.5 (NDC) z=0.6; centre triangle at +-0.25 z=0.2. All CCW in screen space (the
// NDC->screen map preserves winding), so all pass the front-facing `min(edges) >= 0` coverage test.
[[nodiscard]] inline SwRasterScene make_sw_raster_scene()
{
    SwRasterScene s;
    s.cfg.width      = 32;
    s.cfg.height     = 32;
    s.cfg.tri_count  = 3;
    s.cfg.id_bits    = 12;
    s.cfg.local_size = 64;

    const float verts[SwRasterScene::n_vert][4] = {
        {-0.5F, -0.5F, 0.6F, 1.0F}, // 0  quad
        {0.5F, -0.5F, 0.6F, 1.0F},  // 1
        {0.5F, 0.5F, 0.6F, 1.0F},   // 2
        {-0.5F, 0.5F, 0.6F, 1.0F},  // 3
        {-0.25F, -0.25F, 0.2F, 1.0F}, // 4  nearer centre triangle
        {0.25F, -0.25F, 0.2F, 1.0F},  // 5
        {0.0F, 0.25F, 0.2F, 1.0F},    // 6
    };
    for (int v = 0; v < SwRasterScene::n_vert; ++v)
    {
        for (int c = 0; c < 4; ++c) { s.pos[v * 4 + c] = verts[v][c]; }
    }
    const crd::u32 tris[SwRasterScene::n_tri][3] = {{0U, 1U, 2U}, {0U, 2U, 3U}, {4U, 5U, 6U}};
    for (int t = 0; t < SwRasterScene::n_tri; ++t)
    {
        for (int c = 0; c < 3; ++c) { s.idx[t * 3 + c] = tris[t][c]; }
    }
    return s;
}

// CPU ORACLE: run `e` (of `g`) over the scene with f64 buffers (F32-rounded ops), writing the width*height visibility keys
// into `out` (u32). Positions cross as exact f32-valued doubles; the vis cell starts at `kVisEmptyKey`.
inline void sw_raster_oracle(const crd::kir::KGraph& g, const crd::kir::KEntry& e, const SwRasterScene& sc,
                             crd::memory::IAllocator& alloc, crd::containers::Array<crd::u32>& out)
{
    const int npix = static_cast<int>(sc.cfg.width * sc.cfg.height);
    crd::f64  pos64[SwRasterScene::n_vert * 4];
    crd::f64  idx64[SwRasterScene::n_tri * 3];
    for (int i = 0; i < SwRasterScene::n_vert * 4; ++i) { pos64[i] = static_cast<crd::f64>(sc.pos[i]); }
    for (int i = 0; i < SwRasterScene::n_tri * 3; ++i) { idx64[i] = static_cast<crd::f64>(sc.idx[i]); }
    crd::containers::Array<crd::f64> vis64(&alloc);
    vis64.resize(static_cast<crd::usize>(npix), static_cast<crd::f64>(crd::kir::visbuffer::kVisEmptyKey));

    crd::kir::KernelBuffer bufs[3] = {{pos64, SwRasterScene::n_vert * 4, 0, 0},
                                      {idx64, SwRasterScene::n_tri * 3, 0, 1},
                                      {vis64.data(), npix, 0, 2}};
    const crd::u32         grid = (sc.cfg.tri_count + sc.cfg.local_size - 1U) / sc.cfg.local_size;
    crd::kir::eval_cpu_kernel(g, e, bufs, 3, sc.cfg.local_size, &alloc, grid);

    out.resize(static_cast<crd::usize>(npix), 0U);
    for (int i = 0; i < npix; ++i) { out[static_cast<crd::usize>(i)] = static_cast<crd::u32>(vis64[static_cast<crd::usize>(i)]); }
}

// GPU dispatch: bind positions (f32, b0), indices (u32, b1), vis keys (u32, b2, pre-cleared to kVisEmptyKey) as GpuOnly
// device buffers uploaded via staging; dispatch `grid` workgroups; read the vis buffer back into `out` (u32). Mixed dtype
// (f32 + u32) transferred per element type — a u32 key is NOT an f32 value, so the f32-only 1wg harness cannot carry it.
inline void dispatch_visraster(crd::gpu::IComputeContext& ctx, crd::gpu::ComputePipeline& pipe, const SwRasterScene& sc,
                               crd::containers::Array<crd::u32>& out)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    const int      npix     = static_cast<int>(sc.cfg.width * sc.cfg.height);
    const crd::u64 pos_b    = static_cast<crd::u64>(SwRasterScene::n_vert * 4) * sizeof(float);
    const crd::u64 idx_b    = static_cast<crd::u64>(SwRasterScene::n_tri * 3) * sizeof(crd::u32);
    const crd::u64 vis_b    = static_cast<crd::u64>(npix) * sizeof(crd::u32);
    const crd::u32 grid     = (sc.cfg.tri_count + sc.cfg.local_size - 1U) / sc.cfg.local_size;

    auto pos_dev = ctx.create_buffer(pos_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto idx_dev = ctx.create_buffer(idx_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto vis_dev = ctx.create_buffer(vis_b, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
    auto pos_up  = ctx.create_buffer(pos_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto idx_up  = ctx.create_buffer(idx_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto vis_up  = ctx.create_buffer(vis_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto vis_rb  = ctx.create_buffer(vis_b, transfer_dst, g::ComputeMemory::GpuToCpu);

    auto* pp = static_cast<float*>(pos_up->map());
    for (int i = 0; i < SwRasterScene::n_vert * 4; ++i) { pp[i] = sc.pos[i]; }
    pos_up->unmap();
    auto* ip = static_cast<crd::u32*>(idx_up->map());
    for (int i = 0; i < SwRasterScene::n_tri * 3; ++i) { ip[i] = sc.idx[i]; }
    idx_up->unmap();
    auto* vp = static_cast<crd::u32*>(vis_up->map());
    for (int i = 0; i < npix; ++i) { vp[i] = crd::kir::visbuffer::kVisEmptyKey; } // clear to "no triangle"
    vis_up->unmap();

    auto&             rec      = ctx.begin();
    g::ComputeBuffer* binds[3] = {pos_dev.get(), idx_dev.get(), vis_dev.get()};
    rec.copy(*pos_up, *pos_dev, 0U, 0U, pos_b);
    rec.copy(*idx_up, *idx_dev, 0U, 0U, idx_b);
    rec.copy(*vis_up, *vis_dev, 0U, 0U, vis_b);
    rec.barrier(*pos_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*idx_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*vis_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); // the cleared keys must be visible
    rec.dispatch(pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(binds, 3), nullptr, 0U, grid, 1U, 1U);
    rec.barrier(*vis_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*vis_dev, *vis_rb, 0U, 0U, vis_b);
    ctx.submit_and_wait();

    out.resize(static_cast<crd::usize>(npix), 0U);
    const auto* r = static_cast<const crd::u32*>(vis_rb->map());
    for (int i = 0; i < npix; ++i) { out[static_cast<crd::usize>(i)] = r[i]; }
    vis_rb->unmap();
}

// ── B4-vis-2: the DEFERRED ATTRIBUTE INTERPOLATION SHADE (DAIS) scene + oracle + dispatch. ──────────────────────────────

// A single PERSPECTIVE triangle (distinct clip w per vertex) with distinct per-vertex attributes — so the perspective-correct
// interpolation is non-trivial (a plain barycentric blend would give a different, wrong result). The NDC layout is fixed and
// each vertex is pre-multiplied by its w, so the perspective divide recovers the same screen triangle regardless of w.
struct DaisScene
{
    static constexpr int n_vert = 3;
    static constexpr int n_tri  = 1;

    crd::kir::visbuffer::SwRasterConfig      raster_cfg{}; // produces the visibility buffer (tri_count = 1)
    crd::kir::visbuffer::DeferredShadeConfig shade_cfg{};  // the deferred pass (same width/height/id_bits)
    float                                    pos[n_vert * 4] = {};
    crd::u32                                 idx[n_tri * 3]  = {};
    float                                    attr[n_vert]    = {};
};

[[nodiscard]] inline DaisScene make_dais_scene()
{
    DaisScene s;
    s.raster_cfg.width      = 32;
    s.raster_cfg.height     = 32;
    s.raster_cfg.tri_count  = 1;
    s.raster_cfg.id_bits    = 12;
    s.raster_cfg.local_size = 64;
    s.shade_cfg.width       = 32;
    s.shade_cfg.height      = 32;
    s.shade_cfg.id_bits     = 12;
    s.shade_cfg.local_size  = 64;

    const float ndc[DaisScene::n_vert][3] = {{-0.6F, -0.6F, 0.5F}, {0.6F, -0.6F, 0.5F}, {0.0F, 0.6F, 0.5F}};
    const float w[DaisScene::n_vert]      = {1.0F, 2.0F, 4.0F}; // distinct w ⇒ perspective interpolation is exercised
    for (int v = 0; v < DaisScene::n_vert; ++v)
    {
        s.pos[v * 4 + 0] = ndc[v][0] * w[v]; // pre-multiply by w so x/w recovers the NDC — same screen triangle for any w
        s.pos[v * 4 + 1] = ndc[v][1] * w[v];
        s.pos[v * 4 + 2] = ndc[v][2] * w[v];
        s.pos[v * 4 + 3] = w[v];
    }
    s.idx[0]  = 0U;
    s.idx[1]  = 1U;
    s.idx[2]  = 2U;
    s.attr[0] = 2.0F; // distinct exact-f32 vertex attributes
    s.attr[1] = 8.0F;
    s.attr[2] = 32.0F;
    return s;
}

// CPU: rasterize the scene (via the software-rasterizer oracle) into `vis` (u32 keys), so the SAME visibility buffer feeds
// both the GPU deferred pass and the deferred oracle (the rasterizer→vis half is already proven bit-exact in B4-vis-1).
inline void dais_make_vis(const DaisScene& sc, crd::memory::IAllocator& alloc, crd::containers::Array<crd::u32>& vis)
{
    const int      npix = static_cast<int>(sc.raster_cfg.width * sc.raster_cfg.height);
    crd::kir::KGraph rg(&alloc);
    const crd::kir::KEntry re = crd::kir::visbuffer::build_sw_raster_visbuffer(rg, sc.raster_cfg);
    crd::f64        pos64[DaisScene::n_vert * 4];
    crd::f64        idx64[DaisScene::n_tri * 3];
    for (int i = 0; i < DaisScene::n_vert * 4; ++i) { pos64[i] = static_cast<crd::f64>(sc.pos[i]); }
    for (int i = 0; i < DaisScene::n_tri * 3; ++i) { idx64[i] = static_cast<crd::f64>(sc.idx[i]); }
    crd::containers::Array<crd::f64> vis64(&alloc);
    vis64.resize(static_cast<crd::usize>(npix), static_cast<crd::f64>(crd::kir::visbuffer::kVisEmptyKey));
    crd::kir::KernelBuffer rbufs[3] = {{pos64, DaisScene::n_vert * 4, 0, 0},
                                       {idx64, DaisScene::n_tri * 3, 0, 1},
                                       {vis64.data(), npix, 0, 2}};
    const crd::u32         rgrid = (sc.raster_cfg.tri_count + sc.raster_cfg.local_size - 1U) / sc.raster_cfg.local_size;
    crd::kir::eval_cpu_kernel(rg, re, rbufs, 3, sc.raster_cfg.local_size, &alloc, rgrid);
    vis.resize(static_cast<crd::usize>(npix), 0U);
    for (int i = 0; i < npix; ++i) { vis[static_cast<crd::usize>(i)] = static_cast<crd::u32>(vis64[static_cast<crd::usize>(i)]); }
}

// CPU ORACLE for the deferred pass: run `de` (of `dg`) over `vis` + geometry + attributes → the per-pixel shaded f32.
inline void dais_oracle(const crd::kir::KGraph& dg, const crd::kir::KEntry& de, const DaisScene& sc,
                        const crd::containers::Array<crd::u32>& vis, crd::memory::IAllocator& alloc,
                        crd::containers::Array<float>& shade)
{
    const int npix = static_cast<int>(sc.shade_cfg.width * sc.shade_cfg.height);
    crd::f64  pos64[DaisScene::n_vert * 4];
    crd::f64  idx64[DaisScene::n_tri * 3];
    crd::f64  attr64[DaisScene::n_vert];
    for (int i = 0; i < DaisScene::n_vert * 4; ++i) { pos64[i] = static_cast<crd::f64>(sc.pos[i]); }
    for (int i = 0; i < DaisScene::n_tri * 3; ++i) { idx64[i] = static_cast<crd::f64>(sc.idx[i]); }
    for (int i = 0; i < DaisScene::n_vert; ++i) { attr64[i] = static_cast<crd::f64>(sc.attr[i]); }
    crd::containers::Array<crd::f64> vis64(&alloc);
    crd::containers::Array<crd::f64> out64(&alloc);
    vis64.resize(static_cast<crd::usize>(npix), 0.0);
    out64.resize(static_cast<crd::usize>(npix), 0.0);
    for (int i = 0; i < npix; ++i) { vis64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(vis[static_cast<crd::usize>(i)]); }
    crd::kir::KernelBuffer dbufs[5] = {{vis64.data(), npix, 0, 0},
                                       {pos64, DaisScene::n_vert * 4, 0, 1},
                                       {idx64, DaisScene::n_tri * 3, 0, 2},
                                       {attr64, DaisScene::n_vert, 0, 3},
                                       {out64.data(), npix, 0, 4}};
    const crd::u32         dgrid = (static_cast<crd::u32>(npix) + sc.shade_cfg.local_size - 1U) / sc.shade_cfg.local_size;
    crd::kir::eval_cpu_kernel(dg, de, dbufs, 5, sc.shade_cfg.local_size, &alloc, dgrid);
    shade.resize(static_cast<crd::usize>(npix), 0.0F);
    for (int i = 0; i < npix; ++i) { shade[static_cast<crd::usize>(i)] = static_cast<float>(out64[static_cast<crd::usize>(i)]); }
}

// GPU dispatch of the deferred pass: vis (u32, b0), positions (f32, b1), indices (u32, b2), attributes (f32, b3), shaded out
// (f32, b4, pre-cleared 0). `vis` is the CPU-rasterized visibility buffer (same input as the oracle). Reads `out` back.
inline void dispatch_dais(crd::gpu::IComputeContext& ctx, crd::gpu::ComputePipeline& pipe, const DaisScene& sc,
                          const crd::containers::Array<crd::u32>& vis, crd::containers::Array<float>& out)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    const int      npix  = static_cast<int>(sc.shade_cfg.width * sc.shade_cfg.height);
    const crd::u64 vis_b = static_cast<crd::u64>(npix) * sizeof(crd::u32);
    const crd::u64 pos_b = static_cast<crd::u64>(DaisScene::n_vert * 4) * sizeof(float);
    const crd::u64 idx_b = static_cast<crd::u64>(DaisScene::n_tri * 3) * sizeof(crd::u32);
    const crd::u64 att_b = static_cast<crd::u64>(DaisScene::n_vert) * sizeof(float);
    const crd::u64 out_b = static_cast<crd::u64>(npix) * sizeof(float);
    const crd::u32 grid  = (static_cast<crd::u32>(npix) + sc.shade_cfg.local_size - 1U) / sc.shade_cfg.local_size;

    auto vis_dev = ctx.create_buffer(vis_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto pos_dev = ctx.create_buffer(pos_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto idx_dev = ctx.create_buffer(idx_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto att_dev = ctx.create_buffer(att_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto out_dev = ctx.create_buffer(out_b, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
    auto vis_up  = ctx.create_buffer(vis_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto pos_up  = ctx.create_buffer(pos_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto idx_up  = ctx.create_buffer(idx_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto att_up  = ctx.create_buffer(att_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto out_up  = ctx.create_buffer(out_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto out_rb  = ctx.create_buffer(out_b, transfer_dst, g::ComputeMemory::GpuToCpu);

    auto* vp = static_cast<crd::u32*>(vis_up->map());
    for (int i = 0; i < npix; ++i) { vp[i] = vis[static_cast<crd::usize>(i)]; }
    vis_up->unmap();
    auto* pp = static_cast<float*>(pos_up->map());
    for (int i = 0; i < DaisScene::n_vert * 4; ++i) { pp[i] = sc.pos[i]; }
    pos_up->unmap();
    auto* ip = static_cast<crd::u32*>(idx_up->map());
    for (int i = 0; i < DaisScene::n_tri * 3; ++i) { ip[i] = sc.idx[i]; }
    idx_up->unmap();
    auto* ap = static_cast<float*>(att_up->map());
    for (int i = 0; i < DaisScene::n_vert; ++i) { ap[i] = sc.attr[i]; }
    att_up->unmap();
    auto* op = static_cast<float*>(out_up->map());
    for (int i = 0; i < npix; ++i) { op[i] = 0.0F; } // clear: empty pixels keep 0
    out_up->unmap();

    auto&             rec      = ctx.begin();
    g::ComputeBuffer* binds[5] = {vis_dev.get(), pos_dev.get(), idx_dev.get(), att_dev.get(), out_dev.get()};
    rec.copy(*vis_up, *vis_dev, 0U, 0U, vis_b);
    rec.copy(*pos_up, *pos_dev, 0U, 0U, pos_b);
    rec.copy(*idx_up, *idx_dev, 0U, 0U, idx_b);
    rec.copy(*att_up, *att_dev, 0U, 0U, att_b);
    rec.copy(*out_up, *out_dev, 0U, 0U, out_b);
    rec.barrier(*vis_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*pos_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*idx_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*att_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*out_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.dispatch(pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(binds, 5), nullptr, 0U, grid, 1U, 1U);
    rec.barrier(*out_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*out_dev, *out_rb, 0U, 0U, out_b);
    ctx.submit_and_wait();

    out.resize(static_cast<crd::usize>(npix), 0.0F);
    const auto* r = static_cast<const float*>(out_rb->map());
    for (int i = 0; i < npix; ++i) { out[static_cast<crd::usize>(i)] = r[i]; }
    out_rb->unmap();
}

// ── B4-vis-3: the HZB two-pass OCCLUSION CULL scene + oracle + multi-pass dispatch. ────────────────────────────────────────

// A 16x16 depth buffer split into a NEAR occluder wall (left half, depth 0.2) and FAR background (right half, depth 1.0),
// plus 3 clusters: [0] behind the occluder (culled), [1] over open background (visible), [2] in FRONT of the occluder
// (visible). Cluster layout: min_x, min_y, max_x, max_y (screen px), near_depth, mip (host-selected footprint level).
struct HzbScene
{
    static constexpr int n_clusters = 3;

    crd::kir::visbuffer::HzbConfig cfg{};
    float                          depth[16 * 16] = {};      // mip 0 (base_size = 16)
    float                          clusters[n_clusters * 6] = {};
    crd::u32                       expected[n_clusters] = {}; // 1 = visible, 0 = occluded (analytic)
};

[[nodiscard]] inline HzbScene make_hzb_scene()
{
    HzbScene s;
    s.cfg.base_size  = 16;
    s.cfg.local_size = 64;
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x) { s.depth[y * 16 + x] = (x < 8) ? 0.2F : 1.0F; } // near wall | far background
    }
    // cluster 0: footprint (2,2)-(5,5) over the near wall, nearer depth 0.5 (behind the 0.2 wall) → OCCLUDED
    const float defs[HzbScene::n_clusters][6] = {
        {2.0F, 2.0F, 5.0F, 5.0F, 0.5F, 2.0F},   // mip 2: footprint fits ~1 texel of the 4x4 mip
        {10.0F, 2.0F, 13.0F, 5.0F, 0.5F, 2.0F}, // over the far background → VISIBLE
        {2.0F, 2.0F, 5.0F, 5.0F, 0.1F, 2.0F},   // over the wall but IN FRONT of it (0.1 < 0.2) → VISIBLE
    };
    const crd::u32 exp[HzbScene::n_clusters] = {0U, 1U, 1U};
    for (int c = 0; c < HzbScene::n_clusters; ++c)
    {
        for (int f = 0; f < 6; ++f) { s.clusters[c * 6 + f] = defs[c][f]; }
        s.expected[c] = exp[c];
    }
    return s;
}

// CPU ORACLE: build the full HZB pyramid (mip 0 = depth, then each level = max of the 2x2 below, via the SAME downsample
// kernel the GPU runs — one eval per level) then run the cull kernel → the per-cluster visibility flags (u32).
inline void hzb_cull_oracle(const HzbScene& sc, crd::memory::IAllocator& alloc, crd::containers::Array<crd::u32>& vis)
{
    namespace vb        = crd::kir::visbuffer;
    const crd::u32 base = sc.cfg.base_size;
    const crd::u32 nm   = vb::hzb_n_mips(base);
    const int      tot  = static_cast<int>(vb::hzb_total_texels(base));

    crd::containers::Array<crd::f64> hzb(&alloc);
    hzb.resize(static_cast<crd::usize>(tot), 0.0);
    for (int i = 0; i < 16 * 16; ++i) { hzb[static_cast<crd::usize>(i)] = static_cast<crd::f64>(sc.depth[i]); } // mip 0

    for (crd::u32 level = 1U; level < nm; ++level) // build each level in place
    {
        crd::kir::KGraph  g(&alloc);
        const crd::kir::KEntry e   = vb::build_hzb_downsample(g, sc.cfg, level);
        const crd::u32    dst_n = (base >> level) * (base >> level);
        crd::kir::KernelBuffer kb[1] = {{hzb.data(), tot, 0, 0}};
        const crd::u32         grid  = (dst_n + sc.cfg.local_size - 1U) / sc.cfg.local_size;
        crd::kir::eval_cpu_kernel(g, e, kb, 1, sc.cfg.local_size, &alloc, grid);
    }

    crd::containers::Array<crd::f64> offs(&alloc);
    crd::containers::Array<crd::f64> clu(&alloc);
    crd::containers::Array<crd::f64> vis64(&alloc);
    offs.resize(static_cast<crd::usize>(nm), 0.0);
    for (crd::u32 m = 0; m < nm; ++m) { offs[static_cast<crd::usize>(m)] = static_cast<crd::f64>(vb::hzb_mip_offset(base, m)); }
    clu.resize(static_cast<crd::usize>(HzbScene::n_clusters * 6), 0.0);
    for (int i = 0; i < HzbScene::n_clusters * 6; ++i) { clu[static_cast<crd::usize>(i)] = static_cast<crd::f64>(sc.clusters[i]); }
    vis64.resize(static_cast<crd::usize>(HzbScene::n_clusters), 0.0);

    crd::kir::KGraph       cg(&alloc);
    const crd::kir::KEntry ce = vb::build_cluster_cull(cg, sc.cfg, static_cast<crd::u32>(HzbScene::n_clusters));
    crd::kir::KernelBuffer cbufs[4] = {{hzb.data(), tot, 0, 0},
                                       {offs.data(), static_cast<int>(nm), 0, 1},
                                       {clu.data(), HzbScene::n_clusters * 6, 0, 2},
                                       {vis64.data(), HzbScene::n_clusters, 0, 3}};
    crd::kir::eval_cpu_kernel(cg, ce, cbufs, 4, sc.cfg.local_size, &alloc, 1U);

    vis.resize(static_cast<crd::usize>(HzbScene::n_clusters), 0U);
    for (int c = 0; c < HzbScene::n_clusters; ++c) { vis[static_cast<crd::usize>(c)] = static_cast<crd::u32>(vis64[static_cast<crd::usize>(c)]); }
}

// GPU: build the HZB pyramid (one downsample dispatch per level, barriered on the single hzb buffer) then the cull dispatch,
// via `make_pipe(graph, entry, nbufs)` → a backend pipeline. Writes the per-cluster visibility flags into `vis`.
template <typename MakePipe>
inline void hzb_cull_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe, const HzbScene& sc,
                              crd::memory::IAllocator& alloc, crd::containers::Array<crd::u32>& vis)
{
    namespace g         = crd::gpu;
    namespace vb        = crd::kir::visbuffer;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::u32 base = sc.cfg.base_size;
    const crd::u32 nm   = vb::hzb_n_mips(base);
    const int      tot  = static_cast<int>(vb::hzb_total_texels(base));
    const int      ncl  = HzbScene::n_clusters;

    std::unique_ptr<crd::kir::KGraph>       lg[16];
    crd::kir::KEntry                        le[16];
    std::unique_ptr<g::ComputePipeline>     lp[16];
    for (crd::u32 level = 1U; level < nm; ++level) // one downsample kernel + pipeline per level
    {
        lg[level] = std::make_unique<crd::kir::KGraph>(&alloc);
        le[level] = vb::build_hzb_downsample(*lg[level], sc.cfg, level);
        lp[level] = make_pipe(*lg[level], le[level], 1);
    }
    crd::kir::KGraph       cg(&alloc);
    const crd::kir::KEntry ce      = vb::build_cluster_cull(cg, sc.cfg, static_cast<crd::u32>(ncl));
    auto                   cull_pipe = make_pipe(cg, ce, 4);

    const crd::u64 hzb_b = static_cast<crd::u64>(tot) * sizeof(float);
    const crd::u64 off_b = static_cast<crd::u64>(nm) * sizeof(crd::u32);
    const crd::u64 clu_b = static_cast<crd::u64>(ncl * 6) * sizeof(float);
    const crd::u64 vis_b = static_cast<crd::u64>(ncl) * sizeof(crd::u32);

    auto hzb_dev = ctx.create_buffer(hzb_b, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
    auto off_dev = ctx.create_buffer(off_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto clu_dev = ctx.create_buffer(clu_b, storage | transfer_dst, g::ComputeMemory::GpuOnly);
    auto vis_dev = ctx.create_buffer(vis_b, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
    auto hzb_up  = ctx.create_buffer(hzb_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto off_up  = ctx.create_buffer(off_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto clu_up  = ctx.create_buffer(clu_b, transfer_src, g::ComputeMemory::CpuToGpu);
    auto vis_rb  = ctx.create_buffer(vis_b, transfer_dst, g::ComputeMemory::GpuToCpu);

    auto* hp = static_cast<float*>(hzb_up->map());
    for (int i = 0; i < tot; ++i) { hp[i] = (i < 16 * 16) ? sc.depth[i] : 0.0F; } // mip 0 = depth, rest built on GPU
    hzb_up->unmap();
    auto* ofp = static_cast<crd::u32*>(off_up->map());
    for (crd::u32 m = 0; m < nm; ++m) { ofp[m] = vb::hzb_mip_offset(base, m); }
    off_up->unmap();
    auto* clp = static_cast<float*>(clu_up->map());
    for (int i = 0; i < ncl * 6; ++i) { clp[i] = sc.clusters[i]; }
    clu_up->unmap();

    auto& rec = ctx.begin();
    rec.copy(*hzb_up, *hzb_dev, 0U, 0U, hzb_b);
    rec.copy(*off_up, *off_dev, 0U, 0U, off_b);
    rec.copy(*clu_up, *clu_dev, 0U, 0U, clu_b);
    rec.barrier(*hzb_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*off_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*clu_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    for (crd::u32 level = 1U; level < nm; ++level) // build the pyramid level by level (each reads the previous)
    {
        g::ComputeBuffer* b1[1]  = {hzb_dev.get()};
        const crd::u32    dst_n  = (base >> level) * (base >> level);
        const crd::u32    grid   = (dst_n + sc.cfg.local_size - 1U) / sc.cfg.local_size;
        rec.dispatch(*lp[level], crd::containers::ConstSpan<g::ComputeBuffer*>(b1, 1), nullptr, 0U, grid, 1U, 1U);
        rec.barrier(*hzb_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead); // level L visible to L+1
    }
    g::ComputeBuffer* cb[4] = {hzb_dev.get(), off_dev.get(), clu_dev.get(), vis_dev.get()};
    rec.dispatch(*cull_pipe, crd::containers::ConstSpan<g::ComputeBuffer*>(cb, 4), nullptr, 0U, 1U, 1U, 1U);
    rec.barrier(*vis_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*vis_dev, *vis_rb, 0U, 0U, vis_b);
    ctx.submit_and_wait();

    vis.resize(static_cast<crd::usize>(ncl), 0U);
    const auto* r = static_cast<const crd::u32*>(vis_rb->map());
    for (int c = 0; c < ncl; ++c) { vis[static_cast<crd::usize>(c)] = r[c]; }
    vis_rb->unmap();
}

} // namespace crd::kir_test
