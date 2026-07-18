#pragma once

// ckir_visbuffer.hpp — B4-vis: NANITE-class VIRTUALIZED GEOMETRY, the VISIBILITY BUFFER, authored in CKIR.
//
// The modern deferred pipeline (Burns-Hunt 2013 / Karis Nanite 2021): instead of shading every fragment (a fat G-buffer +
// overdraw), the geometry pass writes only WHICH triangle is visible per pixel — a `(depth, triangleId)` visibility key —
// and a later deferred pass materializes attributes + shades ONCE per visible pixel. This header builds the two frontier
// halves in portable compute:
//
//   1. `build_sw_raster_visbuffer` — the COMPUTE SOFTWARE RASTERIZER (Karis 2021): one thread per triangle, edge-function
//      coverage + barycentric depth, packed into a per-pixel `(depth << idBits) | triangleId` u32 written by ATOMIC-MIN
//      (nearest wins). It beats the HW rasterizer on sub-pixel micro-triangles — no fixed-function per-triangle setup —
//      and it is the piece HW raster cannot do well. atomicMin is order-independent, so the visibility buffer is BIT-EXACT
//      vs the CPU oracle on every backend (no 64-bit atomics needed: the key is a single u32). Because the compute shader
//      does its OWN perspective divide, the depth is backend-independent — the HW clip-space NDC-z convention never applies.
//
//   2. `build_deferred_attr_shade` (B4-vis-2) — Deferred Attribute Interpolation Shading (Schied-Dachsbacher 2015): reads
//      the visibility key, fetches the triangle's 3 vertices, reconstructs perspective-correct barycentrics at the pixel,
//      interpolates a vertex attribute, and writes the shaded value. (Added when its slice lands.)

#include <crd/kir/ckir.hpp>

namespace crd::kir::visbuffer
{

// Software-rasterizer parameters. The visibility key packs `depth` in the high `32 - id_bits` bits and `triangleId` in the
// low `id_bits`, so `atomicMin` selects the nearest triangle (ties → lowest id). `id_bits` bounds the triangle count
// (<= 2^id_bits) and the depth precision (32 - id_bits bits); the defaults (12) give 4096 triangles + 20-bit depth.
struct SwRasterConfig
{
    crd::u32 width      = 64;
    crd::u32 height     = 64;
    crd::u32 tri_count  = 2;
    crd::u32 id_bits    = 12; // low bits of the key = triangle id; high (32 - id_bits) = quantized depth
    crd::u32 local_size = 64; // threads per workgroup (one thread rasterizes one triangle)
};

// The empty/background visibility key (no triangle) — the max u32, so any real triangle's key wins the atomicMin. The test
// pre-clears the vis buffer to this and the oracle initializes its cell to the same value.
inline constexpr crd::u32 kVisEmptyKey = 0xFFFFFFFFU;

// Unpack the depth (quantized, high bits) / triangle id (low bits) from a visibility key. Host-side helpers for the oracle
// + the deferred pass; `kVisEmptyKey` unpacks to (max-depth, max-id) which the reader treats as "no triangle".
[[nodiscard]] inline crd::u32 vis_depth(crd::u32 key, crd::u32 id_bits) noexcept { return key >> id_bits; }
[[nodiscard]] inline crd::u32 vis_id(crd::u32 key, crd::u32 id_bits) noexcept
{
    return key & ((1U << id_bits) - 1U);
}

// Build the compute software-rasterizer kernel. Buffers: clip positions (4 f32/vertex, set0 binding0, read), indices
// (3 u32/triangle, set0 binding1, read), visibility keys (width*height u32, set0 binding2, read_write, pre-cleared to
// kVisEmptyKey). Dispatch `ceil(tri_count / local_size)` workgroups. One thread `tid` rasterizes triangle `tid`:
// perspective-divide the 3 clip corners → screen space, edge-function coverage over the (clamped) bbox, barycentric depth,
// pack + atomicMin. Front-facing (CCW, positive area) only — back faces (area <= 0) fall out of the `min(edges) >= 0` test.
[[nodiscard]] inline crd::kir::KEntry build_sw_raster_visbuffer(crd::kir::KGraph& g, const SwRasterConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     fadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     fsub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     fmul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     fdiv = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     fmin = [&](int a, int b) { return g.binary(k::KOp::Min, a, b); };
    const auto     fmax = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    const int pos = g.buffer_decl(k::DType::F32, 0, 0, false); // clip positions: 4 f32 per vertex (x,y,z,w)
    const int idx = g.buffer_decl(k::DType::U32, 0, 1, false); // indices: 3 u32 per triangle
    const int vis = g.buffer_decl(k::DType::U32, 0, 2, true);  // visibility keys: width*height u32 (pre-cleared to max)

    const double depth_max = static_cast<double>((1U << (32U - cfg.id_bits)) - 1U); // depth quantization range
    const int    wf        = cf(static_cast<double>(cfg.width));
    const int    hf        = cf(static_cast<double>(cfg.height));

    const int mark = g.kernel_stmt_mark();
    const int lid  = g.builtin(k::KBuiltin::LocalInvocationIndex);
    const int wgi  = g.builtin(k::KBuiltin::WorkgroupIndex);
    const int tid  = uadd(umul(wgi, cu(cfg.local_size)), lid); // global triangle id

    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, tid, cu(cfg.tri_count)));

    // Fetch the triangle's 3 vertex indices, then its 3 clip positions (4 floats each).
    const int base = umul(tid, cu(3U));
    const int i0   = g.buffer_load(idx, uadd(base, cu(0U)));
    const int i1   = g.buffer_load(idx, uadd(base, cu(1U)));
    const int i2   = g.buffer_load(idx, uadd(base, cu(2U)));
    const auto load_vertex = [&](int vi, int& sx, int& sy, int& nz) {
        const int b  = umul(vi, cu(4U));
        const int px = g.buffer_load(pos, uadd(b, cu(0U)));
        const int py = g.buffer_load(pos, uadd(b, cu(1U)));
        const int pz = g.buffer_load(pos, uadd(b, cu(2U)));
        const int pw = g.buffer_load(pos, uadd(b, cu(3U)));
        const int nx = fdiv(px, pw); // perspective divide → NDC (backend-independent; done in the shader, not fixed-function)
        const int ny = fdiv(py, pw);
        nz           = fdiv(pz, pw);                                    // NDC depth (input constructed in [0,1])
        sx           = fmul(fadd(fmul(nx, cf(0.5)), cf(0.5)), wf);      // NDC [-1,1] → screen [0,W]
        sy           = fmul(fadd(fmul(ny, cf(0.5)), cf(0.5)), hf);      // NDC [-1,1] → screen [0,H]
    };
    int sx0 = 0;
    int sy0 = 0;
    int nz0 = 0;
    int sx1 = 0;
    int sy1 = 0;
    int nz1 = 0;
    int sx2 = 0;
    int sy2 = 0;
    int nz2 = 0;
    load_vertex(i0, sx0, sy0, nz0);
    load_vertex(i1, sx1, sy1, nz1);
    load_vertex(i2, sx2, sy2, nz2);

    // Screen bbox, clamped to [0,W]x[0,H] and converted to an integer pixel range (floor min, ceil max).
    const auto clampf = [&](int x, int lo, int hi) { return g.ternary(k::KOp::Clamp, x, lo, hi); };
    const int  minxi  = g.cast(clampf(g.unary(k::KOp::Floor, fmin(sx0, fmin(sx1, sx2))), cf(0.0), wf), k::DType::U32);
    const int  maxxi  = g.cast(clampf(g.unary(k::KOp::Ceil, fmax(sx0, fmax(sx1, sx2))), cf(0.0), wf), k::DType::U32);
    const int  minyi  = g.cast(clampf(g.unary(k::KOp::Floor, fmin(sy0, fmin(sy1, sy2))), cf(0.0), hf), k::DType::U32);
    const int  maxyi  = g.cast(clampf(g.unary(k::KOp::Ceil, fmax(sy0, fmax(sy1, sy2))), cf(0.0), hf), k::DType::U32);

    // Twice the signed triangle area (CCW > 0). The edge functions below are the sub-triangle areas; each barycentric is
    // edge/area. A back face (area <= 0) yields a negative min-edge and is culled by the coverage test.
    const int area = fsub(fmul(fsub(sx1, sx0), fsub(sy2, sy0)), fmul(fsub(sy1, sy0), fsub(sx2, sx0)));

    const int fy = g.stmt_for_begin(g.binary(k::KOp::Sub, maxyi, minyi));
    const int ly = g.kernel_loop_var(fy);
    const int py = uadd(minyi, ly);
    const int fx = g.stmt_for_begin(g.binary(k::KOp::Sub, maxxi, minxi));
    const int lx = g.kernel_loop_var(fx);
    const int px = uadd(minxi, lx);

    const int pxf = fadd(g.cast(px, k::DType::F32), cf(0.5)); // pixel-center sample point
    const int pyf = fadd(g.cast(py, k::DType::F32), cf(0.5));
    // Edge functions: w0 = edge(s1,s2,p), w1 = edge(s2,s0,p), w2 = edge(s0,s1,p); edge(a,b,p) = (bx-ax)(py-ay)-(by-ay)(px-ax).
    const int w0 = fsub(fmul(fsub(sx2, sx1), fsub(pyf, sy1)), fmul(fsub(sy2, sy1), fsub(pxf, sx1)));
    const int w1 = fsub(fmul(fsub(sx0, sx2), fsub(pyf, sy2)), fmul(fsub(sy0, sy2), fsub(pxf, sx2)));
    const int w2 = fsub(fmul(fsub(sx1, sx0), fsub(pyf, sy0)), fmul(fsub(sy1, sy0), fsub(pxf, sx0)));
    const int emin = fmin(w0, fmin(w1, w2)); // all edges >= 0 (inside, front-facing) iff the minimum is >= 0

    const int cover = g.stmt_if_begin(g.binary(k::KOp::CmpGe, emin, cf(0.0)));
    // Perspective-correct depth: z/w is affine in screen space, so a plain barycentric interpolation of NDC z is the exact
    // pixel depth (this is why depth buffers store z/w). Quantize to the key's depth field.
    const int l0    = fdiv(w0, area);
    const int l1    = fdiv(w1, area);
    const int l2    = fdiv(w2, area);
    const int depth = fadd(fadd(fmul(l0, nz0), fmul(l1, nz1)), fmul(l2, nz2));
    const int du    = g.cast(g.unary(k::KOp::Floor, fmul(depth, cf(depth_max))), k::DType::U32);
    const int key   = g.binary(k::KOp::BitOr, g.binary(k::KOp::Shl, du, cu(cfg.id_bits)), tid); // (depth << id_bits) | tid
    const int pixel = uadd(umul(py, cu(cfg.width)), px);
    g.stmt_buffer_atomic_min(vis, pixel, key);
    g.stmt_if_end(cover);

    g.stmt_for_end(fx);
    g.stmt_for_end(fy);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// B4-vis-2: DEFERRED ATTRIBUTE INTERPOLATION SHADING (DAIS — Schied & Dachsbacher, HPG 2015). The deferred half of the
// visibility-buffer pipeline: one thread per PIXEL reads the visibility key, and — for a covered pixel — fetches the visible
// triangle's 3 clip positions + per-vertex scalar attributes, reconstructs the PERSPECTIVE-CORRECT barycentric weights at the
// pixel centre, and writes the interpolated attribute. Shading runs ONCE per visible pixel (no overdraw, no fat G-buffer) —
// the point of the visibility buffer. Perspective-correct interpolation: with screen-space barycentrics b_i and clip w_i, the
// weight is (b_i / w_i) normalized — a plain b_i blend would warp the attribute under perspective.
struct DeferredShadeConfig
{
    crd::u32 width      = 32;
    crd::u32 height     = 32;
    crd::u32 id_bits    = 12; // must match the rasterizer that produced the visibility buffer
    crd::u32 local_size = 64; // threads per workgroup (one thread rasterizes one PIXEL)
};

// Build the deferred-shade kernel. Buffers: visibility keys (width*height u32, set0 b0, read — from build_sw_raster_visbuffer),
// clip positions (4 f32/vertex, b1, read), indices (3 u32/triangle, b2, read), per-vertex attributes (1 f32/vertex, b3, read),
// shaded output (width*height f32, b4, read_write, pre-cleared to 0). Dispatch `ceil(width*height / local_size)` workgroups.
// An empty pixel (`kVisEmptyKey`) keeps the cleared 0; a covered pixel gets the perspective-correct interpolated attribute.
[[nodiscard]] inline crd::kir::KEntry build_deferred_attr_shade(crd::kir::KGraph& g, const DeferredShadeConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     fadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     fsub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     fmul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     fdiv = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    const int vis  = g.buffer_decl(k::DType::U32, 0, 0, false); // visibility keys
    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false); // clip positions (4 f32/vertex)
    const int idx  = g.buffer_decl(k::DType::U32, 0, 2, false); // indices (3 u32/triangle)
    const int attr = g.buffer_decl(k::DType::F32, 0, 3, false); // per-vertex scalar attribute
    const int outb = g.buffer_decl(k::DType::F32, 0, 4, true);  // shaded output (pre-cleared to 0)

    const int wf   = cf(static_cast<double>(cfg.width));
    const int hf   = cf(static_cast<double>(cfg.height));
    const int npix = cfg.width * cfg.height;

    const int mark = g.kernel_stmt_mark();
    const int lid  = g.builtin(k::KBuiltin::LocalInvocationIndex);
    const int wgi  = g.builtin(k::KBuiltin::WorkgroupIndex);
    const int tid  = uadd(umul(wgi, cu(cfg.local_size)), lid); // global pixel id

    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, tid, cu(static_cast<crd::u32>(npix))));
    const int key   = g.buffer_load(vis, tid);
    // Covered iff the key is not the empty sentinel. (Guard the fetch: an empty key unpacks to id-mask = an out-of-range id.)
    const int covered = g.stmt_if_begin(g.binary(k::KOp::CmpNe, key, cu(kVisEmptyKey)));

    const int triId = g.binary(k::KOp::BitAnd, key, cu((1U << cfg.id_bits) - 1U));
    const int base  = umul(triId, cu(3U));
    const int i0    = g.buffer_load(idx, uadd(base, cu(0U)));
    const int i1    = g.buffer_load(idx, uadd(base, cu(1U)));
    const int i2    = g.buffer_load(idx, uadd(base, cu(2U)));
    const auto load_vertex = [&](int vi, int& sx, int& sy, int& w) {
        const int b  = umul(vi, cu(4U));
        const int vx = g.buffer_load(pos, uadd(b, cu(0U)));
        const int vy = g.buffer_load(pos, uadd(b, cu(1U)));
        w            = g.buffer_load(pos, uadd(b, cu(3U)));
        sx           = fmul(fadd(fmul(fdiv(vx, w), cf(0.5)), cf(0.5)), wf); // same NDC→screen map as the rasterizer
        sy           = fmul(fadd(fmul(fdiv(vy, w), cf(0.5)), cf(0.5)), hf);
    };
    int sx0 = 0;
    int sy0 = 0;
    int w0  = 0;
    int sx1 = 0;
    int sy1 = 0;
    int w1  = 0;
    int sx2 = 0;
    int sy2 = 0;
    int w2  = 0;
    load_vertex(i0, sx0, sy0, w0);
    load_vertex(i1, sx1, sy1, w1);
    load_vertex(i2, sx2, sy2, w2);

    const int px  = g.binary(k::KOp::Mod, tid, cu(cfg.width));
    const int py  = g.binary(k::KOp::Div, tid, cu(cfg.width));
    const int pxf = fadd(g.cast(px, k::DType::F32), cf(0.5)); // pixel-centre sample (same as the rasterizer)
    const int pyf = fadd(g.cast(py, k::DType::F32), cf(0.5));

    // Edge functions edge(a,b,p) = (bx-ax)(py-ay)-(by-ay)(px-ax): the UNnormalized (× 2·area) barycentric numerators. The
    // 1/area factor cancels between num and denom below, so we never divide by area — the ONLY division is the final normalize.
    const int e0 = fsub(fmul(fsub(sx2, sx1), fsub(pyf, sy1)), fmul(fsub(sy2, sy1), fsub(pxf, sx1))); // edge(s1,s2,p)
    const int e1 = fsub(fmul(fsub(sx0, sx2), fsub(pyf, sy2)), fmul(fsub(sy0, sy2), fsub(pxf, sx2))); // edge(s2,s0,p)
    const int e2 = fsub(fmul(fsub(sx1, sx0), fsub(pyf, sy0)), fmul(fsub(sy1, sy0), fsub(pxf, sx0))); // edge(s0,s1,p)
    // Perspective-correct: attr = Σ(e_i/w_i · a_i) / Σ(e_i/w_i) — screen bary weighted by 1/w (a plain e_i blend warps under
    // perspective). Only the final num/denom is an inexact GPU divide (f32 division is ~2.5 ULP on Vulkan/DX12, not correctly
    // rounded) — everything else is exact mul/add; e_i/w_i is exact whenever w is a power of two.
    const int pw0   = fdiv(e0, w0);
    const int pw1   = fdiv(e1, w1);
    const int pw2   = fdiv(e2, w2);
    const int denom = fadd(fadd(pw0, pw1), pw2);
    const int a0    = g.buffer_load(attr, i0);
    const int a1    = g.buffer_load(attr, i1);
    const int a2    = g.buffer_load(attr, i2);
    const int num   = fadd(fadd(fmul(pw0, a0), fmul(pw1, a1)), fmul(pw2, a2));
    g.stmt_buffer_store(outb, tid, fdiv(num, denom));
    g.stmt_if_end(covered);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── B4-vis-3: GPU-DRIVEN HZB (Hi-Z) TWO-PASS OCCLUSION CULL — the culling that makes a Nanite-class pipeline scale. ────────
//
// A Hi-Z buffer is a mip pyramid of the depth buffer where each texel holds the MAX (farthest) depth of the region below it.
// A cluster is occluded iff its NEAREST point is still behind the farthest already-drawn surface over its screen footprint —
// so a single HZB texel at the right mip conservatively answers "is this whole cluster hidden?" in O(1). The two-pass Nanite
// scheme (draw last-frame-visible → build HZB → test everything → draw the newly-revealed) reuses these two kernels:
//   build_hzb_downsample — one mip level (max of the 2x2 below); dispatched once per level to build the pyramid.
//   build_cluster_cull   — per cluster, sample the HZB over its footprint + compare depths → a visibility flag.
// MAX + compare are order-independent ⇒ both are BIT-EXACT vs the CPU oracle on every backend.

struct HzbConfig
{
    crd::u32 base_size  = 64; // power of two; the depth buffer is base_size x base_size, mip 0 of the pyramid
    crd::u32 local_size = 64;
};

// The pyramid level count (mip 0 = base_size, down to 1x1): e.g. 64 → 7 levels (64,32,16,8,4,2,1).
[[nodiscard]] inline crd::u32 hzb_n_mips(crd::u32 base_size) noexcept
{
    crd::u32 n = 0;
    for (crd::u32 s = base_size; s >= 1U; s >>= 1U) { ++n; }
    return n;
}
// The start index (in a single concatenated HZB buffer) of mip `level`: the sum of all lower mips' texel counts.
[[nodiscard]] inline crd::u32 hzb_mip_offset(crd::u32 base_size, crd::u32 level) noexcept
{
    crd::u32 off = 0;
    for (crd::u32 l = 0; l < level; ++l)
    {
        const crd::u32 s = base_size >> l;
        off += s * s;
    }
    return off;
}
// Total texels across every mip — the size of the concatenated HZB storage buffer.
[[nodiscard]] inline crd::u32 hzb_total_texels(crd::u32 base_size) noexcept
{
    return hzb_mip_offset(base_size, hzb_n_mips(base_size));
}

// Build ONE HZB downsample level (`level` >= 1): each destination texel = max of the 2x2 block in mip `level-1`. The whole
// pyramid lives in ONE concatenated f32 storage buffer (set0 b0, read_write), so this reads mip `level-1` and writes mip
// `level` at baked offsets — dispatch it once per level (1..n_mips-1) with a barrier between (level i must finish before i+1
// reads it). One thread per destination texel; dispatch `ceil(dst_size^2 / local_size)` workgroups.
[[nodiscard]] inline crd::kir::KEntry build_hzb_downsample(crd::kir::KGraph& g, const HzbConfig& cfg, crd::u32 level)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     fmax = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };

    const crd::u32 dst_size = cfg.base_size >> level;
    const crd::u32 src_size = cfg.base_size >> (level - 1U);
    const crd::u32 dst_off  = hzb_mip_offset(cfg.base_size, level);
    const crd::u32 src_off  = hzb_mip_offset(cfg.base_size, level - 1U);
    const crd::u32 dst_n    = dst_size * dst_size;

    const int hzb = g.buffer_decl(k::DType::F32, 0, 0, true); // the whole pyramid (read + write, disjoint mip ranges)

    const int mark = g.kernel_stmt_mark();
    const int tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                          g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, tid, cu(dst_n)));
    const int dx    = g.binary(k::KOp::Mod, tid, cu(dst_size));
    const int dy    = g.binary(k::KOp::Div, tid, cu(dst_size));
    const int sx    = umul(dx, cu(2U));
    const int sy    = umul(dy, cu(2U));
    const int row0  = uadd(cu(src_off), umul(sy, cu(src_size)));
    const int row1  = uadd(cu(src_off), umul(uadd(sy, cu(1U)), cu(src_size)));
    const int d00   = g.buffer_load(hzb, uadd(row0, sx));
    const int d01   = g.buffer_load(hzb, uadd(row0, uadd(sx, cu(1U))));
    const int d10   = g.buffer_load(hzb, uadd(row1, sx));
    const int d11   = g.buffer_load(hzb, uadd(row1, uadd(sx, cu(1U))));
    const int dsti  = uadd(uadd(cu(dst_off), umul(dy, cu(dst_size))), dx);
    g.stmt_buffer_store(hzb, dsti, fmax(fmax(d00, d01), fmax(d10, d11)));
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Per-cluster HZB occlusion test. One thread per cluster reads its screen AABB + nearest depth + the (host-selected) HZB mip,
// samples the 2x2 HZB texels covering the footprint at that mip, and writes visible = (near_depth <= max_hzb_depth) ? 1 : 0 —
// i.e. CULL (0) iff the cluster's nearest point is behind the farthest already-drawn surface (conservative: never culls a
// visible cluster). Buffers: hzb (f32, b0, read), mip offsets (u32 per level, b1, read), clusters (6 f32/cluster: min_x,
// min_y, max_x, max_y, near_depth, mip — b2, read), visibility flags (u32/cluster, b3, write). `mip` is a trivial host-side
// setup value (findMSB of the footprint extent); the GPU does the data-parallel HZB sampling + occlusion decision.
[[nodiscard]] inline crd::kir::KEntry build_cluster_cull(crd::kir::KGraph& g, const HzbConfig& cfg, crd::u32 n_clusters)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     ushr = [&](int a, int b) { return g.binary(k::KOp::Shr, a, b); };
    const auto     fmax = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };

    const int hzb  = g.buffer_decl(k::DType::F32, 0, 0, false); // the pyramid
    const int offs = g.buffer_decl(k::DType::U32, 0, 1, false); // mip start offsets
    const int clu  = g.buffer_decl(k::DType::F32, 0, 2, false); // clusters: 6 f32 each
    const int vis  = g.buffer_decl(k::DType::U32, 0, 3, true);  // visibility flags

    const int mark = g.kernel_stmt_mark();
    const int tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                          g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, tid, cu(n_clusters)));
    const int cb    = umul(tid, cu(6U));
    const int minx  = g.cast(g.buffer_load(clu, uadd(cb, cu(0U))), k::DType::U32); // screen AABB (pixels), stored as f32
    const int miny  = g.cast(g.buffer_load(clu, uadd(cb, cu(1U))), k::DType::U32);
    const int maxx  = g.cast(g.buffer_load(clu, uadd(cb, cu(2U))), k::DType::U32);
    const int maxy  = g.cast(g.buffer_load(clu, uadd(cb, cu(3U))), k::DType::U32);
    const int neard = g.buffer_load(clu, uadd(cb, cu(4U)));                        // nearest cluster depth
    const int mip   = g.cast(g.buffer_load(clu, uadd(cb, cu(5U))), k::DType::U32); // host-selected mip level

    const int off = g.buffer_load(offs, mip);              // start of this mip in the pyramid buffer
    const int mw  = ushr(cu(cfg.base_size), mip);          // mip width = base_size >> mip
    const int tx0 = ushr(minx, mip);                       // footprint texels at this mip
    const int ty0 = ushr(miny, mip);
    const int tx1 = ushr(maxx, mip);
    const int ty1 = ushr(maxy, mip);
    const int r0  = uadd(off, umul(ty0, mw));
    const int r1  = uadd(off, umul(ty1, mw));
    const int s00 = g.buffer_load(hzb, uadd(r0, tx0));
    const int s01 = g.buffer_load(hzb, uadd(r0, tx1));
    const int s10 = g.buffer_load(hzb, uadd(r1, tx0));
    const int s11 = g.buffer_load(hzb, uadd(r1, tx1));
    const int hmax = fmax(fmax(s00, s01), fmax(s10, s11)); // farthest already-drawn surface over the footprint
    // visible iff the cluster's nearest point is NOT behind the farthest occluder (occluded ⇒ near_depth > hmax ⇒ flag 0).
    const int visible = g.ternary(k::KOp::Select, cu(1U), cu(0U), g.binary(k::KOp::CmpLe, neard, hmax));
    g.stmt_buffer_store(vis, tid, visible);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── B4: GPU-DRIVEN MESHLET CULL → INDIRECT DISPATCH — the loop that makes a Nanite pipeline scale without a CPU round-trip. ──
//
// A compute pass tests every meshlet and writes the SURVIVING count straight into an INDIRECT-DISPATCH ARGS buffer
// (`{groupCountX, 1, 1}`), which `vkCmdDrawMeshTasksIndirectEXT` / DX12 `ExecuteIndirect(DISPATCH_MESH)` consumes — so the
// mesh-workgroup count is decided ENTIRELY on the GPU (the culled meshlets never dispatch, and the CPU never learns the
// count). `build_meshlet_cull` is that compute pass: one thread per meshlet, `atomicAdd` the survivor count.

struct MeshletCullConfig
{
    crd::u32 n_meshlets = 8;
    crd::u32 local_size = 64;
};

// The meshlet-cull compute kernel. Buffers: per-meshlet cull keys (u32, set0 b0, read — nonzero = the meshlet is visible) and
// the indirect-dispatch args (u32[3] = {groupCountX, groupCountY, groupCountZ}, set0 b1, read_write, pre-cleared to {0,1,1}).
// One thread per meshlet `atomicAdd`s groupCountX (args[0]) when its meshlet survives — so args[0] ends = the survivor count =
// the number of mesh workgroups the following indirect DrawMeshTasks/DispatchMesh launches. Dispatch ceil(n/local_size) groups.
[[nodiscard]] inline crd::kir::KEntry build_meshlet_cull(crd::kir::KGraph& g, const MeshletCullConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };

    const int keys = g.buffer_decl(k::DType::U32, 0, 0, false); // per-meshlet visibility key (0 = culled)
    const int args = g.buffer_decl(k::DType::U32, 0, 1, true);  // indirect args {gx, gy=1, gz=1}; gx accumulates survivors

    const int mark = g.kernel_stmt_mark();
    const int tid  = g.binary(k::KOp::Add, g.binary(k::KOp::Mul, g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                              g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, tid, cu(cfg.n_meshlets)));
    const int vis   = g.stmt_if_begin(g.binary(k::KOp::CmpNe, g.buffer_load(keys, tid), cu(0U))); // survives?
    g.stmt_buffer_atomic_add(args, cu(0U), cu(1U)); // count this survivor into groupCountX — the GPU-driven dispatch count
    g.stmt_if_end(vis);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::visbuffer
