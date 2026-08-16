#pragma once

// ckir_light_cull_test.hpp — CEIR-18a-2 Stage 2b: the shared both-backend harness for the RE-AUTHORED Forward+/clustered
// light-cull PRODUCER (crd::kir::build_cluster_light_cull → assets/ckir/scene_light_cull.ckir). A DETERMINISTIC froxel
// scene (a 4×4 tile grid at num_slices=1 = classic tiled Forward+) + a hand-placed 4-light set is culled on Vulkan
// (GLSL→SPIR-V) AND DX12 (HLSL→DXIL), and the device output — the per-cluster light-index LIST (stride `cap`, survivors
// then `null_index = num_lights` padding, the cooked-consumer layout at lighting_asset.cpp) — is compared to
// `eval_cpu_kernel` BIT-FOR-BIT (serial, no atomics ⇒ ascending order) and to the analytic `expected_list[]` (which locks
// the oracle). The per-cluster COUNT is DERIVED from the list (index of the first null) — the kernel writes no count buffer.
//
// ⛔⛔ Stage 2b re-parameterized the kernel to the SCENE contract: ONE group buffer, section bases read from HEADER WORDS
// (the palette_snapshot indirection form — NOT the 18a-1 four-flat-buffer form). So the harness builds a SYNTHETIC GROUP
// BUFFER: a u32 array whose header words 110/113/114 point at the AABB / light / list sections that follow it. The tile
// AABBs and lights are supplied verbatim to BOTH the GPU dispatch and the oracle (the CEIR-17d bounds discipline).
//
// MARGIN: no light sphere tangent to a tile boundary (small centred spheres, a corner-exact dist²=0 multi-tile light, a far
// light). NON-VACUOUS: 10 empty clusters, a light (L3) culled from EVERY cluster, per-cluster counts unequal.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>       // CEIR-18a-1: load the cull kernel from its committed .ckir asset (ckir_read)
#include <crd/kir/ckir_kernel_eval.hpp> // the CPU oracle

#include <crd/gpu/compute.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>
#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "." // NOLINT(cppcoreguidelines-macro-usage): a build-injected path; the fallback keeps the header standalone-parseable
#endif

namespace crd::kir_test
{

// A 4×4 tile grid (num_slices = 1 ⇒ tiled Forward+); cluster index c = ty*4 + tx spans view-space box x∈[tx,tx+1],
// y∈[ty,ty+1], z∈[0,1]. 4 lights, cap 8. `null_index` = num_lights (an index past every real light) fills the unused tail.
// The synthetic group-buffer layout (words): header[0,128) → AABB[128,224) → light[224,240) → list[240,368).
struct ClusterCullScene
{
    static constexpr int tiles_x       = 4;
    static constexpr int tiles_y       = 4;
    static constexpr int num_slices    = 1;
    static constexpr int num_clusters  = tiles_x * tiles_y * num_slices; // 16
    static constexpr int num_lights    = 4;
    static constexpr int cap           = 8;                              // = max_per_cluster (list stride)
    static constexpr int null_index    = num_lights;                    // sentinel padding (past every real light) = 4
    static constexpr int threads       = 64;
    static constexpr int list_len      = num_clusters * cap;             // 128 u32
    static constexpr int count_len     = num_clusters;                  // 16

    // synthetic group-buffer layout (MUST match build_cluster_light_cull's LightCullParams word indices 110/113/114).
    static constexpr int hdr_words   = 128;                             // header region [0,128): words 110/113/114 valid
    static constexpr int list_word   = 110;                            // header[110] = list section base (= kHdrClusterOff)
    static constexpr int aabb_word   = 113;                            // header[113] = AABB section base (= kHdrFroxelAabbOff)
    static constexpr int light_word  = 114;                            // header[114] = light section base (= kHdrLightViewOff)
    static constexpr int aabb_off    = hdr_words;                       // 128
    static constexpr int aabb_words  = num_clusters * 6;                // 96
    static constexpr int light_off   = aabb_off + aabb_words;           // 224
    static constexpr int light_words = num_lights * 4;                  // 16
    static constexpr int list_off    = light_off + light_words;         // 240
    static constexpr int total_words = list_off + list_len;             // 368

    float aabb[num_clusters * 6] = {}; // per cluster: min.xyz, max.xyz
    float light[num_lights * 4]  = {}; // per light : center.xyz, radius
    int   expected_list[num_clusters][cap] = {}; // the analytic per-cluster list (survivors then null_index)
    int   expected_count[num_clusters]     = {}; // the analytic per-cluster survivor count
};

[[nodiscard]] inline ClusterCullScene make_cluster_cull_scene()
{
    ClusterCullScene s;
    // tile AABBs: 16 unit boxes tiling [0,4]x[0,4]x[0,1].
    for (int ty = 0; ty < ClusterCullScene::tiles_y; ++ty)
    {
        for (int tx = 0; tx < ClusterCullScene::tiles_x; ++tx)
        {
            const int c = ty * ClusterCullScene::tiles_x + tx;
            s.aabb[c * 6 + 0] = static_cast<float>(tx);
            s.aabb[c * 6 + 1] = static_cast<float>(ty);
            s.aabb[c * 6 + 2] = 0.0F;
            s.aabb[c * 6 + 3] = static_cast<float>(tx + 1);
            s.aabb[c * 6 + 4] = static_cast<float>(ty + 1);
            s.aabb[c * 6 + 5] = 1.0F;
        }
    }
    // lights: {cx,cy,cz,r}. L0 small+centred (margin ≥0.3) ⇒ one tile. L1 sits EXACTLY on the (2,2) grid corner ⇒ dist²=0 to
    // the 4 tiles meeting there (c5,c6,c9,c10). L2 → the far corner c15. L3 is far ⇒ culled from EVERY cluster.
    const float lights[ClusterCullScene::num_lights][4] = {
        {0.5F, 0.5F, 0.5F, 0.2F},       // L0 → c0  (tile 0,0)
        {2.0F, 2.0F, 0.5F, 0.3F},       // L1 → c5,c6,c9,c10 (corner-exact)
        {3.5F, 3.5F, 0.5F, 0.2F},       // L2 → c15 (tile 3,3)
        {100.0F, 100.0F, 100.0F, 1.0F}, // L3 → none
    };
    for (int i = 0; i < ClusterCullScene::num_lights; ++i)
    {
        for (int k = 0; k < 4; ++k) { s.light[i * 4 + k] = lights[i][k]; }
    }

    // analytic expected lists: survivors ascending (POINT indices, 0-based), then null_index padding; count = survivors.
    for (int c = 0; c < ClusterCullScene::num_clusters; ++c)
    {
        for (int k = 0; k < ClusterCullScene::cap; ++k) { s.expected_list[c][k] = ClusterCullScene::null_index; }
        s.expected_count[c] = 0;
    }
    const auto set = [&](int c, std::initializer_list<int> ids) {
        int n = 0;
        for (int id : ids) { s.expected_list[c][n] = id; ++n; }
        s.expected_count[c] = n;
    };
    set(0, {0});
    set(5, {1});
    set(6, {1});
    set(9, {1});
    set(10, {1});
    set(15, {2});
    return s;
}

// ⭐⭐ CEIR-18b: the 3D CLUSTERED scene — a 4×4×4 = 64-cluster froxel grid with REAL per-slice z-bounds (the exponential
// boundary table {0.5,1,2,4,8}, ratio 2 — near slices thin, far slices thick). Cluster c = tz*16 + ty*4 + tx (the SAME
// tx + ty*grid[0] + slice*grid[0]*grid[1] the FS computes). Identical member NAMES to ClusterCullScene, so the templated
// harness serves both — the ONLY differences are the counts, the section sizes (AABB 384, list 512), and the z-bounds.
// The kernel is scene_light_cull_3d.ckir (num_clusters 16→64). The kit proves the cull PRODUCER, not the FS binning: it
// pins device == eval_cpu_kernel == analytic on a scene whose lights DISCRIMINATE by depth.
struct ClusterCullScene3D
{
    static constexpr int tiles_x      = 4;
    static constexpr int tiles_y      = 4;
    static constexpr int num_slices   = 4;
    static constexpr int num_clusters = tiles_x * tiles_y * num_slices; // 64
    static constexpr int num_lights   = 4;
    static constexpr int cap          = 8;
    static constexpr int null_index   = num_lights; // 4
    static constexpr int threads      = 64;         // == num_clusters ⇒ one workgroup covers every cluster
    static constexpr int list_len     = num_clusters * cap;             // 512
    static constexpr int count_len    = num_clusters;                   // 64

    static constexpr int hdr_words   = 128;
    static constexpr int list_word   = 110; // = kHdrClusterOff
    static constexpr int aabb_word   = 113; // = kHdrFroxelAabbOff
    static constexpr int light_word  = 114; // = kHdrLightViewOff
    static constexpr int aabb_off    = hdr_words;             // 128
    static constexpr int aabb_words  = num_clusters * 6;      // 384
    static constexpr int light_off   = aabb_off + aabb_words; // 512
    static constexpr int light_words = num_lights * 4;        // 16
    static constexpr int list_off    = light_off + light_words; // 528
    static constexpr int total_words = list_off + list_len;     // 1040

    float aabb[num_clusters * 6] = {};
    float light[num_lights * 4]  = {};
    int   expected_list[num_clusters][cap] = {};
    int   expected_count[num_clusters]     = {};
};

[[nodiscard]] inline ClusterCullScene3D make_cluster_cull_scene_3d()
{
    ClusterCullScene3D s;
    // the exponential z-slice boundaries (N+1 = 5): slice tz spans z ∈ [zb[tz], zb[tz+1]] — the SAME table the renderer will
    // publish to the header words (here supplied verbatim to the AABBs, the CEIR-17d bounds discipline).
    const float zb[ClusterCullScene3D::num_slices + 1] = {0.5F, 1.0F, 2.0F, 4.0F, 8.0F};
    for (int tz = 0; tz < ClusterCullScene3D::num_slices; ++tz)
    {
        for (int ty = 0; ty < ClusterCullScene3D::tiles_y; ++ty)
        {
            for (int tx = 0; tx < ClusterCullScene3D::tiles_x; ++tx)
            {
                const int c = tz * (ClusterCullScene3D::tiles_x * ClusterCullScene3D::tiles_y)
                              + ty * ClusterCullScene3D::tiles_x + tx;
                s.aabb[c * 6 + 0] = static_cast<float>(tx);
                s.aabb[c * 6 + 1] = static_cast<float>(ty);
                s.aabb[c * 6 + 2] = zb[tz];
                s.aabb[c * 6 + 3] = static_cast<float>(tx + 1);
                s.aabb[c * 6 + 4] = static_cast<float>(ty + 1);
                s.aabb[c * 6 + 5] = zb[tz + 1];
            }
        }
    }
    // lights DISCRIMINATE by depth. L0/L1 share screen tile (0,0) but sit at DIFFERENT depths ⇒ DIFFERENT z-slices (a z-blind
    // cull would bin both into slice 0). L2 sits EXACTLY on the slice2/slice3 z-boundary (z=4) ⇒ dist²=0 to BOTH ⇒ two
    // clusters (the froxel-overlap case). L3 is far ⇒ culled from every cluster (non-vacuous). 60 clusters stay empty.
    const float lights[ClusterCullScene3D::num_lights][4] = {
        {0.5F, 0.5F, 0.75F, 0.2F}, // L0 → tile(0,0) z=0.75∈slice0 ⇒ c0
        {0.5F, 0.5F, 3.0F, 0.2F},  // L1 → tile(0,0) z=3.0 ∈slice2 ⇒ c32 (SAME tile as L0, deeper slice)
        {2.5F, 2.5F, 4.0F, 0.1F},  // L2 → tile(2,2) z=4.0 = slice2|slice3 boundary ⇒ c42 AND c58
        {50.0F, 50.0F, 50.0F, 1.0F}, // L3 → none
    };
    for (int i = 0; i < ClusterCullScene3D::num_lights; ++i)
    {
        for (int k = 0; k < 4; ++k) { s.light[i * 4 + k] = lights[i][k]; }
    }
    for (int c = 0; c < ClusterCullScene3D::num_clusters; ++c)
    {
        for (int k = 0; k < ClusterCullScene3D::cap; ++k) { s.expected_list[c][k] = ClusterCullScene3D::null_index; }
        s.expected_count[c] = 0;
    }
    const auto set = [&](int c, std::initializer_list<int> ids) {
        int n = 0;
        for (int id : ids) { s.expected_list[c][n] = id; ++n; }
        s.expected_count[c] = n;
    };
    set(0, {0});   // L0: tile(0,0) slice0
    set(32, {1});  // L1: tile(0,0) slice2 — z-discrimination proof
    set(42, {2});  // L2: tile(2,2) slice2 (z=4 lower face)
    set(58, {2});  // L2: tile(2,2) slice3 (z=4 upper face)
    return s;
}

// bit-reinterpret a float as its u32 storage word (the pull-shader convention the kernel reads back via int_bits_to_float).
[[nodiscard]] inline crd::u32 cull_fbits(float f)
{
    crd::u32 u = 0U;
    std::memcpy(&u, &f, 4);
    return u;
}

// assemble the SYNTHETIC GROUP BUFFER (u32): header words 110/113/114 → the section bases, then AABB / light / list.
// ⭐ CEIR-18b: templated on the SCENE so the same harness serves the 2D tiled (16 clusters) and 3D clustered (64) scenes —
// both carry the identical member names, differing only in counts + the per-slice z-bounds baked into `aabb[]`.
template <typename Scene>
inline void build_cull_group_buffer(const Scene& sc, crd::containers::Array<crd::u32>& out)
{
    out.resize(static_cast<crd::usize>(Scene::total_words), 0U);
    out[Scene::list_word]  = static_cast<crd::u32>(Scene::list_off);
    out[Scene::aabb_word]  = static_cast<crd::u32>(Scene::aabb_off);
    out[Scene::light_word] = static_cast<crd::u32>(Scene::light_off);
    for (int i = 0; i < Scene::aabb_words; ++i)
    {
        out[static_cast<crd::usize>(Scene::aabb_off + i)] = cull_fbits(sc.aabb[i]);
    }
    for (int i = 0; i < Scene::light_words; ++i)
    {
        out[static_cast<crd::usize>(Scene::light_off + i)] = cull_fbits(sc.light[i]);
    }
    // list section [list_off, total) stays 0 (the kernel overwrites every slot).
}

// derive the per-cluster survivor count from the list: the index of the first `null_index` in each cluster's run.
template <typename Scene>
inline void cull_counts_from_list(const crd::containers::Array<crd::u32>& list, crd::containers::Array<crd::u32>& count_out)
{
    count_out.resize(static_cast<crd::usize>(Scene::count_len), 0U);
    for (int c = 0; c < Scene::num_clusters; ++c)
    {
        int n = 0;
        for (int k = 0; k < Scene::cap; ++k)
        {
            if (static_cast<int>(list[static_cast<crd::usize>(c * Scene::cap + k)]) == Scene::null_index)
            {
                break;
            }
            ++n;
        }
        count_out[static_cast<crd::usize>(c)] = static_cast<crd::u32>(n);
    }
}

// CEIR-18a-2 Stage 2b: LOAD the cull kernel from its committed `.ckir` ASSET (assets/ckir/scene_light_cull.ckir). The C++
// builder `build_cluster_light_cull` is KEPT IN-TREE (the regen source); `ckir_read`/`ckir_write` round-trips byte-exact.
// ⭐ CEIR-18b: `path` defaults to the 2D tiled asset; the 3D clustered gate passes scene_light_cull_3d.ckir (the same builder
// re-parameterized to 64 clusters). Both round-trip byte-exact; the ONLY on-disk difference is the num_clusters guard const.
[[nodiscard]] inline bool read_cull_ckir(crd::kir::KGraph& g, crd::kir::KEntry& e, crd::memory::IAllocator& alloc,
                                         const char* path = CRD_REPO_DIR "/assets/ckir/scene_light_cull.ckir")
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) { return false; }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) { return false; }
    f.seekg(0);
    crd::containers::Array<char> src(&alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    const auto rr = crd::kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e);
    return rr.ok;
}

// CPU ORACLE: run the kernel over the SYNTHETIC GROUP BUFFER as an f64 buffer holding u32 VALUES (offsets as integers,
// floats as their u32 BIT PATTERNS — apply_unary models IntBitsToFloat, so the reinterpret matches the device). One buffer,
// one workgroup of `threads`. Read the list section back; derive counts from it.
template <typename Scene>
inline void cull_oracle(const crd::kir::KGraph& g, const crd::kir::KEntry& e, const Scene& sc,
                        crd::memory::IAllocator& alloc, crd::containers::Array<crd::u32>& list_out,
                        crd::containers::Array<crd::u32>& count_out)
{
    crd::containers::Array<crd::u32> group(&alloc);
    build_cull_group_buffer(sc, group);
    crd::containers::Array<crd::f64> buf64(&alloc);
    buf64.resize(static_cast<crd::usize>(Scene::total_words), 0.0);
    for (int i = 0; i < Scene::total_words; ++i)
    {
        buf64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(group[static_cast<crd::usize>(i)]);
    }
    crd::kir::KernelBuffer kb{buf64.data(), Scene::total_words, 0, 0};
    crd::kir::eval_cpu_kernel(g, e, &kb, 1, static_cast<crd::u32>(Scene::threads), &alloc, 1U);

    list_out.resize(static_cast<crd::usize>(Scene::list_len), 0U);
    for (int i = 0; i < Scene::list_len; ++i)
    {
        list_out[static_cast<crd::usize>(i)] =
            static_cast<crd::u32>(buf64[static_cast<crd::usize>(Scene::list_off + i)]);
    }
    cull_counts_from_list<Scene>(list_out, count_out);
}

// GPU dispatch: ONE group buffer (u32, set 0 binding 0); dispatch 1 workgroup; read the list section back. `make_pipe(graph,
// entry, nbufs)` → a backend pipeline (nbufs = 1 — the single group buffer).
template <typename Scene, typename MakePipe>
inline void cull_dispatch(crd::gpu::IComputeContext& ctx, MakePipe make_pipe, const crd::kir::KGraph& g,
                          const crd::kir::KEntry& e, const Scene& sc, crd::containers::Array<crd::u32>& list_out,
                          crd::containers::Array<crd::u32>& count_out)
{
    namespace gp = crd::gpu;
    using gp::compute_usage::storage;
    using gp::compute_usage::transfer_dst;
    using gp::compute_usage::transfer_src;

    const crd::u64 grp_b   = static_cast<crd::u64>(Scene::total_words) * sizeof(crd::u32);
    const crd::u64 list_b  = static_cast<crd::u64>(Scene::list_len) * sizeof(crd::u32);
    const crd::u64 list_bo = static_cast<crd::u64>(Scene::list_off) * sizeof(crd::u32);

    auto pipe = make_pipe(g, e, 1);
    if (pipe == nullptr)
    {
        // a failed emit/compile ⇒ return a POISON list so the caller's REQUIRE(emit_ok) fails LOUD (never a null-deref
        // SIGSEGV in the dispatch below — the first-device-run trap when the new kernel form fails to emit).
        list_out.resize(static_cast<crd::usize>(Scene::list_len), 0xFFFFFFFFU);
        count_out.resize(static_cast<crd::usize>(Scene::count_len), 0xFFFFFFFFU);
        return;
    }

    auto grp_dev = ctx.create_buffer(grp_b, storage | transfer_dst | transfer_src, gp::ComputeMemory::GpuOnly);
    auto grp_up  = ctx.create_buffer(grp_b, transfer_src, gp::ComputeMemory::CpuToGpu);
    auto list_rb = ctx.create_buffer(list_b, transfer_dst, gp::ComputeMemory::GpuToCpu);

    // fill the synthetic group buffer straight into the mapped upload buffer (header words → section bases, then bits).
    auto* gp0 = static_cast<crd::u32*>(grp_up->map());
    for (int i = 0; i < Scene::total_words; ++i) { gp0[i] = 0U; }
    gp0[Scene::list_word]  = static_cast<crd::u32>(Scene::list_off);
    gp0[Scene::aabb_word]  = static_cast<crd::u32>(Scene::aabb_off);
    gp0[Scene::light_word] = static_cast<crd::u32>(Scene::light_off);
    for (int i = 0; i < Scene::aabb_words; ++i) { gp0[Scene::aabb_off + i] = cull_fbits(sc.aabb[i]); }
    for (int i = 0; i < Scene::light_words; ++i) { gp0[Scene::light_off + i] = cull_fbits(sc.light[i]); }
    grp_up->unmap();

    auto&              rec      = ctx.begin();
    gp::ComputeBuffer* binds[1] = {grp_dev.get()};
    rec.copy(*grp_up, *grp_dev, 0U, 0U, grp_b);
    rec.barrier(*grp_dev, gp::ComputeAccess::TransferDst, gp::ComputeAccess::ShaderRead);
    rec.dispatch(*pipe, crd::containers::ConstSpan<gp::ComputeBuffer*>(binds, 1), nullptr, 0U, 1U, 1U, 1U);
    rec.barrier(*grp_dev, gp::ComputeAccess::ShaderWrite, gp::ComputeAccess::TransferSrc);
    rec.copy(*grp_dev, *list_rb, list_bo, 0U, list_b);
    ctx.submit_and_wait();

    list_out.resize(static_cast<crd::usize>(Scene::list_len), 0U);
    const auto* rl = static_cast<const crd::u32*>(list_rb->map());
    for (int i = 0; i < Scene::list_len; ++i) { list_out[static_cast<crd::usize>(i)] = rl[i]; }
    list_rb->unmap();
    cull_counts_from_list<Scene>(list_out, count_out);
}

} // namespace crd::kir_test
