#pragma once

// ckir_mesh.hpp — D-007 B19-c2: MESH EXTRACTION from depth+normal maps (the bridge from a captured radiance field
// into real triangle geometry, and thence the B1 material/mesh pipeline). The gold-standard route (the one the 2DGS
// paper, Huang et al. 2024, uses): TSDF FUSION → MARCHING CUBES.
//   • TSDF fusion (THIS file, first) — integrate one or more posed depth maps into a Truncated Signed Distance Field on
//     a voxel grid: for each voxel, project it into the view, compare its camera-space depth to the observed surface
//     depth at that pixel, and accumulate a truncated signed distance (running weighted average across views). The
//     zero-level set of the fused field IS the surface.
//   • Marching cubes (next) — extract the SDF=0 isosurface as a triangle mesh.
//
// TSDF is source-agnostic: the depth map can come from the 2DGS ray-surfel render (ckir_gsplat2d.hpp,
// depthSum/(1−T)), an RGBD sensor, or any depth pass. Authored in CKIR (scalar tier ⇒ component-wise) ⇒ all backends.
//
// SIGN CONVENTION: sdf = d_obs − depth(voxel). A voxel CLOSER to the camera than the surface (in free space) has
// sdf>0; one BEHIND the surface (occluded) has sdf<0. The surface is the sdf=0 crossing. Voxels farther behind than
// the truncation μ are not observed by this view (skipped); voxels in front are integrated up to +1.

#include <crd/kir/ckir.hpp>
#include <crd/kir/mc_tables.hpp>

namespace crd::kir::mesh
{

// ── TSDF INTEGRATE: one thread per voxel; accumulate ONE view into (tsdf_sum, w_sum). Call once per posed depth map
//    (the running weighted average is finalised as tsdf_sum/w_sum by the reader / marching cubes).
//
// Buffers:
//   b0 depth   (F32, imgW·imgH): observed SURFACE depth (view-z) per pixel; ≤ 0 ⇒ no surface (background), skipped.
//   b1 camera  (F32, 20): [R00..R22 (row-major 3×3 view rotation) · tx ty tz · fx fy cx cy · near · imgW imgH]
//   b2 gparams (F32, 5): [originX originY originZ · voxel_h · mu]   (grid dims nx,ny,nz are compile-time in the config)
//   b3 tsdf_sum (F32, nx·ny·nz, RMW): Σ w·tsdf
//   b4 w_sum    (F32, nx·ny·nz, RMW): Σ w
struct TsdfConfig
{
    int nx         = 64;
    int ny         = 64;
    int nz         = 64;
    int img_w      = 512;
    int img_h      = 512;
    int local_size = 64;
};

[[nodiscard]] inline KEntry build_tsdf_integrate_kernel(KGraph& g, const TsdfConfig& cfg)
{
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  add = [](KGraph& gg, int a, int b) { return gg.binary(KOp::Add, a, b); };
    const auto  sub = [](KGraph& gg, int a, int b) { return gg.binary(KOp::Sub, a, b); };
    const auto  mul = [](KGraph& gg, int a, int b) { return gg.binary(KOp::Mul, a, b); };
    const auto  dv  = [](KGraph& gg, int a, int b) { return gg.binary(KOp::Div, a, b); };
    const auto  mn  = [](KGraph& gg, int a, int b) { return gg.binary(KOp::Min, a, b); };
    const auto  mx  = [](KGraph& gg, int a, int b) { return gg.binary(KOp::Max, a, b); };

    const int dep_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int cam_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int gp_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int ts_b  = g.buffer_decl(DType::F32, 0, 3, true);
    const int ws_b  = g.buffer_decl(DType::F32, 0, 4, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                           g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const auto cl  = [&](int k) { const int v = g.buffer_load(cam_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const auto gp  = [&](int k) { const int v = g.buffer_load(gp_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };

    // voxel index → (i,j,k)
    const int nx  = cu(static_cast<crd::u32>(cfg.nx));
    const int nxy = cu(static_cast<crd::u32>(cfg.nx * cfg.ny));
    const int vi  = g.binary(KOp::Mod, tid, nx);
    const int vj  = g.binary(KOp::Mod, g.binary(KOp::Div, tid, nx), cu(static_cast<crd::u32>(cfg.ny)));
    const int vk  = g.binary(KOp::Div, tid, nxy);

    const int ox = gp(0);
    const int oy = gp(1);
    const int oz = gp(2);
    const int hh = gp(3);
    const int mu = gp(4);
    // voxel centre in world space  P = origin + (idx + 0.5)·h
    const int px = add(g, ox, mul(g, add(g, g.cast(vi, DType::F32), ks(0.5)), hh));
    const int py = add(g, oy, mul(g, add(g, g.cast(vj, DType::F32), ks(0.5)), hh));
    const int pz = add(g, oz, mul(g, add(g, g.cast(vk, DType::F32), ks(0.5)), hh));

    const int r00 = cl(0); const int r01 = cl(1); const int r02 = cl(2);
    const int r10 = cl(3); const int r11 = cl(4); const int r12 = cl(5);
    const int r20 = cl(6); const int r21 = cl(7); const int r22 = cl(8);
    const int tx = cl(9); const int ty = cl(10); const int tz = cl(11);
    const int fx = cl(12); const int fy = cl(13); const int cx = cl(14); const int cy = cl(15);
    const int near = cl(16);

    // view-space voxel  V = R·P + t
    const int vx = add(g, add(g, add(g, mul(g, r00, px), mul(g, r01, py)), mul(g, r02, pz)), tx);
    const int vy = add(g, add(g, add(g, mul(g, r10, px), mul(g, r11, py)), mul(g, r12, pz)), ty);
    const int vz = add(g, add(g, add(g, mul(g, r20, px), mul(g, r21, py)), mul(g, r22, pz)), tz);
    const int front = g.binary(KOp::CmpGt, vz, near);
    const int vzs   = mx(g, vz, ks(1.0e-6));
    const int inv   = dv(g, ks(1.0), vzs);

    // project to pixel
    const int sx = add(g, mul(g, fx, mul(g, vx, inv)), cx);
    const int sy = add(g, mul(g, fy, mul(g, vy, inv)), cy);
    const int ix = g.cast(g.unary(KOp::Floor, sx), DType::U32);
    const int iy = g.cast(g.unary(KOp::Floor, sy), DType::U32);
    const int in_x = g.binary(KOp::BitAnd, g.binary(KOp::CmpGe, sx, ks(0.0)), g.binary(KOp::CmpLt, sx, ks(static_cast<double>(cfg.img_w))));
    const int in_y = g.binary(KOp::BitAnd, g.binary(KOp::CmpGe, sy, ks(0.0)), g.binary(KOp::CmpLt, sy, ks(static_cast<double>(cfg.img_h))));
    const int in_img = g.binary(KOp::BitAnd, in_x, in_y);
    // clamped pixel index for a safe inline load (masked out below when out of image)
    const int pcl = mn(g, g.binary(KOp::Add, ix, mul(g, iy, cu(static_cast<crd::u32>(cfg.img_w)))), cu(static_cast<crd::u32>(cfg.img_w * cfg.img_h - 1)));
    const int d_obs = g.buffer_load(dep_b, pcl);
    g.stmt_materialize(d_obs);

    const int has_surf = g.binary(KOp::CmpGt, d_obs, ks(1.0e-6)); // a surface exists at that pixel
    const int sdf_raw  = sub(g, d_obs, vz);
    const int not_occl = g.binary(KOp::CmpGt, sdf_raw, g.unary(KOp::Neg, mu)); // not farther behind than μ
    const int tsdf     = mn(g, mx(g, dv(g, sdf_raw, mx(g, mu, ks(1.0e-9))), ks(-1.0)), ks(1.0));
    const int wv       = ks(1.0);

    const int keep = g.binary(KOp::BitAnd, g.binary(KOp::BitAnd, front, in_img), g.binary(KOp::BitAnd, has_surf, not_occl));
    const int add_t = g.select(keep, mul(g, tsdf, wv), ks(0.0));
    const int add_w = g.select(keep, wv, ks(0.0));

    // RMW accumulate (per-voxel thread owns its slot ⇒ race-free; materialise the loads before the stores)
    const int t_old = g.buffer_load(ts_b, tid);
    const int w_old = g.buffer_load(ws_b, tid);
    g.stmt_materialize(t_old);
    g.stmt_materialize(w_old);
    g.stmt_buffer_store(ts_b, tid, add(g, t_old, add_t));
    g.stmt_buffer_store(ws_b, tid, add(g, w_old, add_w));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── SURFACE DEPTH: turn a splat render's G-buffer into the depth map the TSDF consumes. The 2DGS render accumulates an
//    α-weighted depth SUM and a transmittance T (ckir_gsplat2d.hpp), so the surface depth is depthSum/(1−T); a pixel
//    with no surface (T≈1) is written 0 (⇒ the TSDF skips it). One thread per pixel.
//   b0 render (F32, 8/pixel: [R G B T · depthSum · Nx Ny Nz])  ·  b1 depth (F32, 1/pixel, write)
[[nodiscard]] inline KEntry build_surface_depth_kernel(KGraph& g, int local_size = 64)
{
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int rnd_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int dep_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(local_size))),
                           g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int base = g.binary(KOp::Mul, tid, cu(8U));
    const int tval = g.buffer_load(rnd_b, g.binary(KOp::Add, base, cu(3U)));
    const int dsum = g.buffer_load(rnd_b, g.binary(KOp::Add, base, cu(4U)));
    g.stmt_materialize(tval);
    g.stmt_materialize(dsum);
    const int acc = g.binary(KOp::Sub, ks(1.0), tval); // accumulated opacity
    const int has = g.binary(KOp::CmpGt, acc, ks(1.0e-4));
    const int sd  = g.select(has, g.binary(KOp::Div, dsum, g.binary(KOp::Max, acc, ks(1.0e-9))), ks(0.0));
    g.stmt_buffer_store(dep_b, tid, sd);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// B19-c2b — MARCHING CUBES (Lorensen & Cline 1987): extract the SDF=0 isosurface of the fused field as a triangle mesh.
// Standard variable-output pipeline (same shape as the B19-a4 splat bin): per cell → cubeindex → triangle COUNT, then
// SCAN (ckir_scan) the counts → per-cell output offset + total, then EMIT each cell's triangles (edge zero-crossing
// interpolation + gradient normals) at its offset. The corner/edge tables are compile-time constants baked into the
// kernels; only the 256×16 triTable is a runtime buffer (indexed by the per-cell cubeindex).
//
// Convention (matches TSDF): a corner is INSIDE (bit set) iff field < 0. Vertex on edge (a,b) = lerp at the field zero:
// t = field[a]/(field[a]−field[b]). Cells are (nx−1)·(ny−1)·(nz−1); grid dims are compile-time (McConfig).
struct McConfig
{
    int nx         = 64;
    int ny         = 64;
    int nz         = 64;
    int local_size = 64;
};

// FINALISE: fused accumulators → the scalar field marching cubes reads. field = w>0 ? tsdf_sum/w_sum : +1 (unobserved =
// free space, so it never spawns a spurious surface interior). One thread per voxel.
//   b0 tsdf_sum (F32) · b1 w_sum (F32) · b2 field (F32, write)
[[nodiscard]] inline KEntry build_tsdf_finalize_kernel(KGraph& g, int local_size = 64)
{
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int ts_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int ws_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int fd_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid  = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(local_size))),
                          g.builtin(KBuiltin::LocalInvocationIndex));
    const int mark = g.kernel_stmt_mark();
    const int ws   = g.buffer_load(ws_b, tid);
    const int ts   = g.buffer_load(ts_b, tid);
    g.stmt_materialize(ws);
    g.stmt_materialize(ts);
    const int obs  = g.binary(KOp::CmpGt, ws, ks(0.5));
    const int fld  = g.select(obs, g.binary(KOp::Div, ts, g.binary(KOp::Max, ws, ks(1.0e-9))), ks(1.0));
    g.stmt_buffer_store(fd_b, tid, fld);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

namespace detail
{
// the world-space voxel index of cell corner `c` (compile-time offsets), given cell coords (ci,cj,ck).
[[nodiscard]] inline int corner_voxel(KGraph& g, int ci, int cj, int ck, int c, int nx, int nxy)
{
    const auto cu = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const int  ox = kMcCornerOff[c * 3 + 0];
    const int  oy = kMcCornerOff[c * 3 + 1];
    const int  oz = kMcCornerOff[c * 3 + 2];
    const int  xx = g.binary(KOp::Add, ci, cu(static_cast<crd::u32>(ox)));
    const int  yy = g.binary(KOp::Add, cj, cu(static_cast<crd::u32>(oy)));
    const int  zz = g.binary(KOp::Add, ck, cu(static_cast<crd::u32>(oz)));
    return g.binary(KOp::Add, g.binary(KOp::Add, xx, g.binary(KOp::Mul, yy, cu(static_cast<crd::u32>(nx)))),
                    g.binary(KOp::Mul, zz, cu(static_cast<crd::u32>(nxy))));
}

// the cubeindex (8-bit inside/outside mask) of a cell, and its 8 corner field values materialised into fld[8].
[[nodiscard]] inline int cube_index(KGraph& g, int fd_b, int ci, int cj, int ck, int nx, int nxy, int* fld)
{
    const auto cu = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto ks = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    int idx = cu(0U);
    for (int c = 0; c < 8; ++c)
    {
        const int v = g.buffer_load(fd_b, corner_voxel(g, ci, cj, ck, c, nx, nxy));
        g.stmt_materialize(v);
        fld[c] = v;
        const int inside = g.binary(KOp::CmpLt, v, ks(0.0));
        idx = g.binary(KOp::Add, idx, g.select(inside, cu(static_cast<crd::u32>(1U << c)), cu(0U)));
    }
    g.stmt_materialize(idx);
    return idx;
}
} // namespace detail

// COUNT: per cell → number of triangles marching cubes will emit (from the triTable, via the cubeindex).
//   b0 field (F32) · b1 triTable (I32, 256·16) · b2 count (F32, 1/cell, write)
[[nodiscard]] inline KEntry build_mc_count_kernel(KGraph& g, const McConfig& cfg)
{
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };

    const int fd_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int tt_b = g.buffer_decl(DType::I32, 0, 1, false);
    const int ct_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid  = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                          g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark  = g.kernel_stmt_mark();
    const int cnx   = cfg.nx - 1;
    const int cny   = cfg.ny - 1;
    const int nxy   = cfg.nx * cfg.ny;
    const int ci    = g.binary(KOp::Mod, tid, cu(static_cast<crd::u32>(cnx)));
    const int cj    = g.binary(KOp::Mod, g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(cnx))), cu(static_cast<crd::u32>(cny)));
    const int ck    = g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(cnx * cny)));
    int fld[8];
    const int idx = detail::cube_index(g, fd_b, ci, cj, ck, cfg.nx, nxy, fld);
    const int row = g.binary(KOp::Mul, idx, cu(16U));
    // numtris = Σ_{t=0}^{4} (triTable[row + 3t] ≥ 0)
    int n = cu(0U);
    for (int t = 0; t < 5; ++t)
    {
        const int e0 = g.buffer_load(tt_b, g.binary(KOp::Add, row, cu(static_cast<crd::u32>(t * 3))));
        n = g.binary(KOp::Add, n, g.select(g.binary(KOp::CmpGe, e0, g.constant(0.0, shu, DType::I32)), cu(1U), cu(0U)));
    }
    g.stmt_buffer_store(ct_b, tid, g.cast(n, DType::F32)); // F32 so the scan (F32) can prefix-sum it

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

namespace detail
{
// the world-space position of the surface vertex on cube edge `e` (runtime), by linear interpolation to the field zero.
// Fills v[0..2]. `ox,oy,oz,hh` are the pre-loaded grid origin + voxel size; `ec_b`/`co_b`/`fd_b` the edge/corner/field
// buffers; (ci,cj,ck) the cell coords.
inline void edge_vertex(KGraph& g, int fd_b, int ec_b, int co_b, int ox, int oy, int oz, int hh,
                        int ci, int cj, int ck, int nx, int nxy, int e, int* v)
{
    const auto cu  = [&](crd::u32 val) { return g.constant(static_cast<double>(val), make_shape({1}), DType::U32); };
    const auto ks  = [&](double val) { return g.constant(val, make_shape({1}), DType::F32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int e2  = mul(e, cu(2U));
    const int ca  = g.buffer_load(ec_b, e2);              // corner a
    const int cb  = g.buffer_load(ec_b, add(e2, cu(1U))); // corner b
    g.stmt_materialize(ca);
    g.stmt_materialize(cb);
    const int a3 = mul(ca, cu(3U));
    const int b3 = mul(cb, cu(3U));
    const int axo = g.buffer_load(co_b, a3);
    const int ayo = g.buffer_load(co_b, add(a3, cu(1U)));
    const int azo = g.buffer_load(co_b, add(a3, cu(2U)));
    const int bxo = g.buffer_load(co_b, b3);
    const int byo = g.buffer_load(co_b, add(b3, cu(1U)));
    const int bzo = g.buffer_load(co_b, add(b3, cu(2U)));
    const int xa = add(ci, axo);
    const int ya = add(cj, ayo);
    const int za = add(ck, azo);
    const int xb = add(ci, bxo);
    const int yb = add(cj, byo);
    const int zb = add(ck, bzo);
    const int via = add(add(xa, mul(ya, cu(static_cast<crd::u32>(nx)))), mul(za, cu(static_cast<crd::u32>(nxy))));
    const int vib = add(add(xb, mul(yb, cu(static_cast<crd::u32>(nx)))), mul(zb, cu(static_cast<crd::u32>(nxy))));
    const int fa = g.buffer_load(fd_b, via);
    const int fb = g.buffer_load(fd_b, vib);
    g.stmt_materialize(fa);
    g.stmt_materialize(fb);
    // interpolation parameter t = fa/(fa−fb) at the zero crossing (guard the degenerate equal-value edge)
    const int den  = sub(fa, fb);
    const int dens = g.select(g.binary(KOp::CmpGt, g.unary(KOp::Abs, den), ks(1.0e-9)), den, ks(1.0));
    const int tv   = g.binary(KOp::Div, fa, dens);
    // world positions of the two corners:  P = origin + (voxel_idx + 0.5)·h
    const int pax = add(ox, mul(add(g.cast(xa, DType::F32), ks(0.5)), hh));
    const int pay = add(oy, mul(add(g.cast(ya, DType::F32), ks(0.5)), hh));
    const int paz = add(oz, mul(add(g.cast(za, DType::F32), ks(0.5)), hh));
    const int pbx = add(ox, mul(add(g.cast(xb, DType::F32), ks(0.5)), hh));
    const int pby = add(oy, mul(add(g.cast(yb, DType::F32), ks(0.5)), hh));
    const int pbz = add(oz, mul(add(g.cast(zb, DType::F32), ks(0.5)), hh));
    v[0] = add(pax, mul(tv, sub(pbx, pax)));
    v[1] = add(pay, mul(tv, sub(pby, pay)));
    v[2] = add(paz, mul(tv, sub(pbz, paz)));
    g.stmt_materialize(v[0]);
    g.stmt_materialize(v[1]);
    g.stmt_materialize(v[2]);
}
} // namespace detail

// EMIT: per cell, write its triangles (positions + outward face normals) at the scanned output offset. One thread per
// cell; each of the ≤5 possible triangles is written only if valid (guarded ⇒ never spills into the next cell's range).
//   b0 field (F32) · b1 gparams (F32,4: [ox oy oz h]) · b2 triTable (I32) · b3 edgeConn (U32,24) · b4 cornerOff (U32,24)
//   b5 cellOffset (F32, 1/cell — the exclusive-scanned triangle offset) · b6 out (F32, 18/triangle: 3×[pos.xyz · n.xyz])
[[nodiscard]] inline KEntry build_mc_emit_kernel(KGraph& g, const McConfig& cfg)
{
    using detail::edge_vertex;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int fd_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int gp_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int tt_b = g.buffer_decl(DType::I32, 0, 2, false);
    const int ec_b = g.buffer_decl(DType::U32, 0, 3, false);
    const int co_b = g.buffer_decl(DType::U32, 0, 4, false);
    const int of_b = g.buffer_decl(DType::F32, 0, 5, false);
    const int ou_b = g.buffer_decl(DType::F32, 0, 6, true);
    const int tid  = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                          g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int cnx  = cfg.nx - 1;
    const int cny  = cfg.ny - 1;
    const int nxy  = cfg.nx * cfg.ny;
    const int ci   = g.binary(KOp::Mod, tid, cu(static_cast<crd::u32>(cnx)));
    const int cj   = g.binary(KOp::Mod, g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(cnx))), cu(static_cast<crd::u32>(cny)));
    const int ck   = g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(cnx * cny)));
    g.stmt_materialize(ci); g.stmt_materialize(cj); g.stmt_materialize(ck);
    const int ox = g.buffer_load(gp_b, cu(0U));
    const int oy = g.buffer_load(gp_b, cu(1U));
    const int oz = g.buffer_load(gp_b, cu(2U));
    const int hh = g.buffer_load(gp_b, cu(3U));
    g.stmt_materialize(ox); g.stmt_materialize(oy); g.stmt_materialize(oz); g.stmt_materialize(hh);

    int fld[8];
    const int idx = detail::cube_index(g, fd_b, ci, cj, ck, cfg.nx, nxy, fld);
    const int row = mul(idx, cu(16U));
    const int base = g.cast(g.buffer_load(of_b, tid), DType::U32); // first output triangle index for this cell
    g.stmt_materialize(base);

    for (int t = 0; t < 5; ++t)
    {
        const int ea = g.buffer_load(tt_b, add(row, cu(static_cast<crd::u32>(t * 3 + 0))));
        g.stmt_materialize(ea);
        const int valid = g.binary(KOp::CmpGe, ea, g.constant(0.0, shu, DType::I32));
        const int cif = g.stmt_if_begin(valid);
        {
            const int eb = g.cast(g.buffer_load(tt_b, add(row, cu(static_cast<crd::u32>(t * 3 + 1)))), DType::U32);
            const int ec = g.cast(g.buffer_load(tt_b, add(row, cu(static_cast<crd::u32>(t * 3 + 2)))), DType::U32);
            const int eau = g.cast(ea, DType::U32);
            int v0[3];
            int v1[3];
            int v2[3];
            edge_vertex(g, fd_b, ec_b, co_b, ox, oy, oz, hh, ci, cj, ck, cfg.nx, nxy, eau, v0);
            edge_vertex(g, fd_b, ec_b, co_b, ox, oy, oz, hh, ci, cj, ck, cfg.nx, nxy, eb, v1);
            edge_vertex(g, fd_b, ec_b, co_b, ox, oy, oz, hh, ci, cj, ck, cfg.nx, nxy, ec, v2);
            // outward face normal = normalize((v1−v0) × (v2−v0)) (triTable winding ⇒ points from inside to outside)
            const int e1x = sub(v1[0], v0[0]);
            const int e1y = sub(v1[1], v0[1]);
            const int e1z = sub(v1[2], v0[2]);
            const int e2x = sub(v2[0], v0[0]);
            const int e2y = sub(v2[1], v0[1]);
            const int e2z = sub(v2[2], v0[2]);
            const int nrx = sub(mul(e1y, e2z), mul(e1z, e2y));
            const int nry = sub(mul(e1z, e2x), mul(e1x, e2z));
            const int nrz = sub(mul(e1x, e2y), mul(e1y, e2x));
            const int nln = g.binary(KOp::Max, g.unary(KOp::Sqrt, add(add(mul(nrx, nrx), mul(nry, nry)), mul(nrz, nrz))), ks(1.0e-12));
            const int ninv = g.binary(KOp::Div, ks(1.0), nln);
            // the canonical triTable winds triangles so cross(v1−v0,v2−v0) points INWARD (toward the field<iso interior).
            // Emit the reversed winding (v0,v2,v1) with the negated normal ⇒ an OUTWARD normal, consistent with the winding.
            const int nx0 = g.unary(KOp::Neg, mul(nrx, ninv));
            const int ny0 = g.unary(KOp::Neg, mul(nry, ninv));
            const int nz0 = g.unary(KOp::Neg, mul(nrz, ninv));
            g.stmt_materialize(nx0); g.stmt_materialize(ny0); g.stmt_materialize(nz0);
            const int obase = mul(add(base, cu(static_cast<crd::u32>(t))), cu(18U));
            const auto wv = [&](int slot, int val) { g.stmt_buffer_store(ou_b, add(obase, cu(static_cast<crd::u32>(slot))), val); };
            wv(0, v0[0]); wv(1, v0[1]); wv(2, v0[2]); wv(3, nx0); wv(4, ny0); wv(5, nz0);
            wv(6, v2[0]); wv(7, v2[1]); wv(8, v2[2]); wv(9, nx0); wv(10, ny0); wv(11, nz0);
            wv(12, v1[0]); wv(13, v1[1]); wv(14, v1[2]); wv(15, nx0); wv(16, ny0); wv(17, nz0);
        }
        g.stmt_if_end(cif);
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::mesh
