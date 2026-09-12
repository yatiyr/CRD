#pragma once

// ckir_ddgi.hpp — D-007 B14-b: DYNAMIC DIFFUSE GLOBAL ILLUMINATION (Majercik et al. 2019, "Dynamic Diffuse Global
// Illumination with Ray-Traced Irradiance Fields"), authored in CKIR — the no-RT-HARDWARE GI tier of B14. A regular grid of
// light probes; each probe stores an OCTAHEDRAL irradiance map (incoming radiance integrated over directions) + an
// octahedral DEPTH-MOMENT map (mean depth, mean depth² per direction) that drives a Chebyshev/variance occlusion test so the
// GI does NOT leak through walls. The per-probe RAY GENERATION (radiance + hit distance per ray) is the renderer leaf,
// deferred to B9 ray tracing (or a rasterized probe cube); the ANALYTIC CORE — octahedral encode/decode, the irradiance +
// depth-moment integration & temporal blend, and the trilinear + Chebyshev + normal-weighted probe SAMPLING — is built and
// verified here (bit-exact/ULP CPU oracle + both backends), exactly as SVGF was.
//
// Everything is SCALARIZED (dir.x/y/z as separate F32 nodes) so it lowers cleanly through the statement-tier compute
// emitters (which are scalar — vec ops live on the fragment path). FP32 `precise` ⇒ bit-matches the oracle; the only ULP is
// `sqrt` in the normalize (IEEE-correctly-rounded, so effectively exact).

#include <crd/kir/ckir.hpp>
#include <crd/units/units.hpp> // Length<f64> for the probe-grid world positions/spacing (two-layer typed API, ADR-0078)

namespace crd::kir::ddgi
{

namespace detail
{
[[nodiscard]] inline int kf(KGraph& g, crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); }
[[nodiscard]] inline int sgn(KGraph& g, int a) { return g.select(g.binary(KOp::CmpGe, a, kf(g, 0.0)), kf(g, 1.0), kf(g, -1.0)); } // ±1 (0→+1)
[[nodiscard]] inline int absv(KGraph& g, int a) { return g.unary(KOp::Abs, a); }
} // namespace detail

// OCTAHEDRAL ENCODE — a unit direction (dx,dy,dz) → oct coordinate (ox,oy) ∈ [−1,1]² (Cigolle 2014). Projects onto the
// octahedron by the L1 norm, then folds the −Z hemisphere out to the corners so the whole sphere maps to the square.
inline void oct_encode(KGraph& g, int dx, int dy, int dz, int& ox, int& oy)
{
    using namespace detail;
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const int  s   = add(add(absv(g, dx), absv(g, dy)), absv(g, dz));   // |x|+|y|+|z|
    const int  px  = g.binary(KOp::Div, dx, s);
    const int  py  = g.binary(KOp::Div, dy, s);
    const int  neg = g.binary(KOp::CmpLt, dz, kf(g, 0.0));              // lower hemisphere?
    ox             = g.select(neg, mul(sub(kf(g, 1.0), absv(g, py)), sgn(g, px)), px);
    oy             = g.select(neg, mul(sub(kf(g, 1.0), absv(g, px)), sgn(g, py)), py);
}

// OCTAHEDRAL DECODE — oct coordinate (ox,oy) ∈ [−1,1]² → unit direction (dx,dy,dz). The inverse fold + an L2 normalize.
inline void oct_decode(KGraph& g, int ox, int oy, int& dx, int& dy, int& dz)
{
    using namespace detail;
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const int  z0  = sub(sub(kf(g, 1.0), absv(g, ox)), absv(g, oy));   // 1−|x|−|y|
    const int  neg = g.binary(KOp::CmpLt, z0, kf(g, 0.0));
    dx             = g.select(neg, mul(sub(kf(g, 1.0), absv(g, oy)), sgn(g, ox)), ox);
    dy             = g.select(neg, mul(sub(kf(g, 1.0), absv(g, ox)), sgn(g, oy)), oy);
    dz             = z0;
    const int len  = g.unary(KOp::Sqrt, add(add(mul(dx, dx), mul(dy, dy)), mul(dz, dz)));
    dx             = g.binary(KOp::Div, dx, len);
    dy             = g.binary(KOp::Div, dy, len);
    dz             = g.binary(KOp::Div, dz, len);
}

// CHEBYSHEV VISIBILITY — the DDGI depth-moment occlusion test. Given a probe's stored (mean, mean²) of hit distance toward a
// surface + the actual surface distance `dist`, return a soft visibility ∈ [0,1]: 1 when the surface is at/nearer than the
// mean (lit), falling off by Chebyshev's inequality `σ²/(σ² + (dist−mean)²)` when farther (occluded ⇒ no leak). Schied-style
// variance shadows applied to the irradiance field.
[[nodiscard]] inline int chebyshev(KGraph& g, int mean, int mean2, int dist)
{
    using namespace detail;
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const int  var  = g.binary(KOp::Max, sub(mean2, mul(mean, mean)), kf(g, 0.0)); // σ² = E[d²]−E[d]²
    const int  d    = sub(dist, mean);
    const int  cheb = g.binary(KOp::Div, var, add(var, mul(d, d)));                // σ²/(σ²+Δ²)
    // lit if dist ≤ mean (Δ ≤ 0) ⇒ visibility 1; else the Chebyshev bound
    return g.select(g.binary(KOp::CmpLe, dist, mean), kf(g, 1.0), cheb);
}

// Sample a probe's octahedral map (nearest texel) in a direction — returns component `c` of an `R×R×comps` tile at
// `probe_base`. dir → oct (ox,oy)∈[−1,1]² → uv∈[0,1]² → nearest texel (round + clamp). (Bilinear is a later refinement.)
[[nodiscard]] inline int sample_oct_nearest(KGraph& g, int map_buf, int probe_base, int dx, int dy, int dz, int r, int comps, int c)
{
    const auto kf  = [&](crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    int        ox  = 0;
    int        oy  = 0;
    oct_encode(g, dx, dy, dz, ox, oy);
    // uv = oct*0.5+0.5 ∈ [0,1]; texel = round(uv*(R−1)) clamped to [0,R−1]
    const auto texel = [&](int o) {
        const int uvf = mul(add(mul(o, kf(0.5)), kf(0.5)), kf(static_cast<double>(r - 1)));
        const int rf  = g.unary(KOp::Floor, add(uvf, kf(0.5)));
        const int cl  = g.binary(KOp::Max, g.binary(KOp::Min, rf, kf(static_cast<double>(r - 1))), kf(0.0));
        return g.cast(cl, DType::U32);
    };
    const int idx = add(add(probe_base, mul(add(mul(texel(oy), ku(static_cast<crd::u32>(r))), texel(ox)), ku(static_cast<crd::u32>(comps)))), ku(static_cast<crd::u32>(c)));
    return g.buffer_load(map_buf, idx);
}

// BILINEAR octahedral sample — the gold quality version (smooth vs the blocky nearest). Continuous texel coord → 4 corner
// taps (clamp-to-edge) → bilinear blend. (The 1-texel octahedral-seam mirror border is the last-mile refinement.)
[[nodiscard]] inline int sample_oct_bilinear(KGraph& g, int map_buf, int probe_base, int dx, int dy, int dz, int r, int comps, int c)
{
    const auto kf  = [&](crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    int        ox  = 0;
    int        oy  = 0;
    oct_encode(g, dx, dy, dz, ox, oy);
    const int fx = mul(add(mul(ox, kf(0.5)), kf(0.5)), kf(static_cast<double>(r - 1))); // ∈[0,R−1]
    const int fy = mul(add(mul(oy, kf(0.5)), kf(0.5)), kf(static_cast<double>(r - 1)));
    const int x0 = g.unary(KOp::Floor, fx);
    const int y0 = g.unary(KOp::Floor, fy);
    const int wx = sub(fx, x0);
    const int wy = sub(fy, y0);
    const auto clampu = [&](int v) {
        return g.cast(g.binary(KOp::Max, g.binary(KOp::Min, v, kf(static_cast<double>(r - 1))), kf(0.0)), DType::U32);
    };
    const int cx0 = clampu(x0);
    const int cx1 = clampu(add(x0, kf(1.0)));
    const int cy0 = clampu(y0);
    const int cy1 = clampu(add(y0, kf(1.0)));
    const auto tap = [&](int tx, int ty) {
        const int idx = add(add(probe_base, mul(add(mul(ty, ku(static_cast<crd::u32>(r))), tx), ku(static_cast<crd::u32>(comps)))), ku(static_cast<crd::u32>(c)));
        return g.buffer_load(map_buf, idx);
    };
    const int top = add(tap(cx0, cy0), mul(wx, sub(tap(cx1, cy0), tap(cx0, cy0)))); // lerp along x, row y0
    const int bot = add(tap(cx0, cy1), mul(wx, sub(tap(cx1, cy1), tap(cx0, cy1)))); // lerp along x, row y1
    return add(top, mul(wy, sub(bot, top)));                                        // lerp along y
}

// Configuration for the DDGI probe SAMPLE (B14-b-2). A single 2×2×2 probe cell (the 8 corners of the unit cube [0,1]³) — the
// minimal verifiable trilinear case; a full grid is the same math indexed by cell. `oct_res` = the octahedral map resolution.
struct DdgiConfig
{
    int    oct_res         = 8;    // R (irradiance + depth maps are R×R)
    int    num_rays        = 64;   // rays traced per probe per frame (the RT leaf produces dir+radiance+dist)
    double hysteresis      = 0.03; // temporal blend of the new estimate into the probe (small ⇒ stable, slow to react) — dimensionless
    double depth_sharpness = 50.0; // crd-lint-allow-untagged-physical: dimensionless cosine power for the depth-moment gather (an exponent, not a physical quantity)
    // the probe GRID (a full field, not just one cell). Probe (px,py,pz) is at origin + (px,py,pz)·spacing; flat index
    // pidx = (pz·grid_y + py)·grid_x + px. The sample clamps the containing cell to [0, grid−2] so its +1 corner exists.
    int                 grid_x  = 2;
    int                 grid_y  = 2;
    int                 grid_z  = 2;
    crd::units::Length64 origin_x{0.0}; // probe-grid origin (world metres)
    crd::units::Length64 origin_y{0.0};
    crd::units::Length64 origin_z{0.0};
    crd::units::Length64 spacing{1.0};  // probe spacing (world metres)

    [[nodiscard]] int  probe_count() const noexcept { return grid_x * grid_y * grid_z; }
    [[nodiscard]] bool valid() const noexcept { return oct_res >= 2 && num_rays >= 1 && grid_x >= 2 && grid_y >= 2 && grid_z >= 2; }
};

// Build the DDGI probe SAMPLE — the shading-time indirect-diffuse lookup (Majercik 2019 §4). For each surface query (world
// pos in the unit cell + normal), blend the 8 corner probes: trilinear weight × normal "wrap" weight `((n·d̂_probe+1)/2)²`
// (down-weight probes behind the surface) × CHEBYSHEV visibility (down-weight probes occluded per the depth moments) ×
// irradiance sampled octahedrally in the NORMAL direction. out = Σw·E / Σw ⇒ leak-free indirect diffuse. Deterministic.
// Buffers: 0=pos(F32 N·3), 1=normal(F32 N·3), 2=probe_irr(F32 8·R·R·3), 3=probe_depth(F32 8·R·R·2), 4=out(F32 N·3, out).
[[nodiscard]] inline KEntry build_ddgi_sample(KGraph& g, const DdgiConfig& cfg)
{
    const int  r    = cfg.oct_res;
    const int  irr_n = r * r * 3; // per-probe irradiance tile size
    const int  dpt_n = r * r * 2; // per-probe depth tile size
    const auto kf   = [&](crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int pos_b   = g.buffer_decl(DType::F32, 0, 0, false);
    const int nrm_b   = g.buffer_decl(DType::F32, 0, 1, false);
    const int irr_b   = g.buffer_decl(DType::F32, 0, 2, false);
    const int dpt_b   = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_b   = g.buffer_decl(DType::F32, 0, 4, true);
    const int p       = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int p3      = mul(p, ku(3));
    const int f0      = kf(0.0);

    const int mark = g.kernel_stmt_mark();
    const int px = g.buffer_load(pos_b, p3);
    const int py = g.buffer_load(pos_b, add(p3, ku(1)));
    const int pz = g.buffer_load(pos_b, add(p3, ku(2)));
    const int nx = g.buffer_load(nrm_b, p3);
    const int ny = g.buffer_load(nrm_b, add(p3, ku(1)));
    const int nz = g.buffer_load(nrm_b, add(p3, ku(2)));

    // locate the containing cell in the probe grid; clamp so the +1 corner exists; keep the trilinear fractions.
    const double si  = 1.0 / cfg.spacing.value; // .value: SI metres — raw at the IR boundary (ADR-0078)
    const int    lx  = mul(sub(px, kf(cfg.origin_x.value)), kf(si)); // local (probe-index-space) coords
    const int    ly  = mul(sub(py, kf(cfg.origin_y.value)), kf(si));
    const int    lz  = mul(sub(pz, kf(cfg.origin_z.value)), kf(si));
    const auto   cell = [&](int lc, int dim) { return fmax(g.binary(KOp::Min, g.unary(KOp::Floor, lc), kf(static_cast<double>(dim - 2))), f0); };
    const int    cxf = cell(lx, cfg.grid_x);
    const int    cyf = cell(ly, cfg.grid_y);
    const int    czf = cell(lz, cfg.grid_z);
    const int    fx  = sub(lx, cxf);
    const int    fy  = sub(ly, cyf);
    const int    fz  = sub(lz, czf);
    const int    cxu = g.cast(cxf, DType::U32);
    const int    cyu = g.cast(cyf, DType::U32);
    const int    czu = g.cast(czf, DType::U32);

    int sir = f0;
    int sig = f0;
    int sib = f0;
    int sw  = f0;
    for (int pi = 0; pi < 8; ++pi)
    {
        const int dxi = (pi & 1) ? 1 : 0;
        const int dyi = (pi & 2) ? 1 : 0;
        const int dzi = (pi & 4) ? 1 : 0;
        // trilinear weight
        const int wx  = dxi ? fx : sub(kf(1.0), fx);
        const int wy  = dyi ? fy : sub(kf(1.0), fy);
        const int wz  = dzi ? fz : sub(kf(1.0), fz);
        const int tri = mul(mul(wx, wy), wz);
        // probe flat index + world position
        const int pidx  = add(mul(add(mul(add(czu, ku(static_cast<crd::u32>(dzi))), ku(static_cast<crd::u32>(cfg.grid_y))), add(cyu, ku(static_cast<crd::u32>(dyi)))), ku(static_cast<crd::u32>(cfg.grid_x))), add(cxu, ku(static_cast<crd::u32>(dxi))));
        const int ppx   = add(kf(cfg.origin_x.value), mul(add(cxf, kf(static_cast<double>(dxi))), kf(cfg.spacing.value)));
        const int ppy   = add(kf(cfg.origin_y.value), mul(add(cyf, kf(static_cast<double>(dyi))), kf(cfg.spacing.value)));
        const int ppz   = add(kf(cfg.origin_z.value), mul(add(czf, kf(static_cast<double>(dzi))), kf(cfg.spacing.value)));
        // direction surface → probe (+ distance), normalized
        const int ex   = sub(ppx, px);
        const int ey   = sub(ppy, py);
        const int ez   = sub(ppz, pz);
        const int dist = g.unary(KOp::Sqrt, fmax(add(add(mul(ex, ex), mul(ey, ey)), mul(ez, ez)), kf(1.0e-12)));
        const int inv  = g.binary(KOp::Div, kf(1.0), add(dist, kf(1.0e-6)));
        const int ux   = mul(ex, inv);
        const int uy   = mul(ey, inv);
        const int uz   = mul(ez, inv);
        // normal wrap weight: ((n·d̂+1)/2)²  (probes in front of the surface dominate)
        const int ndot  = add(add(mul(nx, ux), mul(ny, uy)), mul(nz, uz));
        const int wrap0 = mul(add(ndot, kf(1.0)), kf(0.5));
        const int wrap  = mul(fmax(wrap0, f0), fmax(wrap0, f0));
        // Chebyshev visibility from the depth moments (BILINEAR, toward the probe)
        const int dbase = mul(pidx, ku(static_cast<crd::u32>(dpt_n)));
        const int mean  = sample_oct_bilinear(g, dpt_b, dbase, ux, uy, uz, r, 2, 0);
        const int mean2 = sample_oct_bilinear(g, dpt_b, dbase, ux, uy, uz, r, 2, 1);
        const int vis   = chebyshev(g, mean, mean2, dist);
        const int w     = add(mul(mul(tri, wrap), vis), kf(1.0e-6)); // tiny ε so a fully-occluded cell still normalizes
        // irradiance BILINEAR-sampled in the NORMAL direction
        const int ibase = mul(pidx, ku(static_cast<crd::u32>(irr_n)));
        sir             = add(sir, mul(w, sample_oct_bilinear(g, irr_b, ibase, nx, ny, nz, r, 3, 0)));
        sig             = add(sig, mul(w, sample_oct_bilinear(g, irr_b, ibase, nx, ny, nz, r, 3, 1)));
        sib             = add(sib, mul(w, sample_oct_bilinear(g, irr_b, ibase, nx, ny, nz, r, 3, 2)));
        sw              = add(sw, w);
    }
    g.stmt_buffer_store(out_b, p3, g.binary(KOp::Div, sir, sw));
    g.stmt_buffer_store(out_b, add(p3, ku(1)), g.binary(KOp::Div, sig, sw));
    g.stmt_buffer_store(out_b, add(p3, ku(2)), g.binary(KOp::Div, sib, sw));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the DDGI PROBE UPDATE (B14-b-3) — integrate this frame's rays into each probe's octahedral irradiance + depth-moment
// maps, then temporally blend into the previous probe (Majercik 2019 §3.2). One thread per (probe, texel): the texel's
// direction d = oct_decode(texel center); accumulate over the probe's rays — irradiance gets a cosine `max(0,d·rd)` weight,
// the depth moments get a sharper `cos^depth_sharpness` weight (crisp occlusion) of the hit distance and its square; then
// `out = lerp(prev, new, hysteresis)`. The per-ray dir/radiance/dist is the RT leaf (deferred to B9); the integration is
// here. Buffers: 0=ray_dir(F32 8·rays·3), 1=ray_rad(F32 8·rays·3), 2=ray_dist(F32 8·rays), 3=prev_irr(F32 8·R·R·3),
// 4=prev_dpt(F32 8·R·R·2), 5=out_irr(F32 8·R·R·3, out), 6=out_dpt(F32 8·R·R·2, out). local_size 64; grid = 8·R·R/64.
[[nodiscard]] inline KEntry build_ddgi_probe_update(KGraph& g, const DdgiConfig& cfg)
{
    const int  r    = cfg.oct_res;
    const int  nr   = cfg.num_rays;
    const auto kf   = [&](crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int rdir_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int rrad_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int rdst_b = g.buffer_decl(DType::F32, 0, 2, false);
    const int pirr_b = g.buffer_decl(DType::F32, 0, 3, false);
    const int pdpt_b = g.buffer_decl(DType::F32, 0, 4, false);
    const int oirr_b = g.buffer_decl(DType::F32, 0, 5, true);
    const int odpt_b = g.buffer_decl(DType::F32, 0, 6, true);
    const int idx    = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int f0     = kf(0.0);

    const int mark  = g.kernel_stmt_mark();
    const int probe = g.binary(KOp::Div, idx, ku(static_cast<crd::u32>(r * r)));      // which probe
    const int texel = g.binary(KOp::Sub, idx, mul(probe, ku(static_cast<crd::u32>(r * r)))); // texel within it
    const int tx    = g.binary(KOp::Mod, texel, ku(static_cast<crd::u32>(r)));
    const int ty    = g.binary(KOp::Div, texel, ku(static_cast<crd::u32>(r)));
    // texel center → oct coord ∈ [−1,1] → direction
    const int oxv = sub(mul(divv(add(g.cast(tx, DType::F32), kf(0.5)), kf(static_cast<double>(r))), kf(2.0)), kf(1.0));
    const int oyv = sub(mul(divv(add(g.cast(ty, DType::F32), kf(0.5)), kf(static_cast<double>(r))), kf(2.0)), kf(1.0));
    int       dx  = 0;
    int       dy  = 0;
    int       dz  = 0;
    oct_decode(g, oxv, oyv, dx, dy, dz);

    const int rbase3 = mul(mul(probe, ku(static_cast<crd::u32>(nr))), ku(3)); // 3·(probe·nr)
    const int rbase1 = mul(probe, ku(static_cast<crd::u32>(nr)));
    int sir = f0;
    int sig = f0;
    int sib = f0;
    int sw = f0;
    int smean = f0;
    int smean2 = f0;
    int sdw = f0;
    for (int ray = 0; ray < nr; ++ray)
    {
        const int b3   = add(rbase3, ku(static_cast<crd::u32>(ray * 3)));
        const int rdx  = g.buffer_load(rdir_b, b3);
        const int rdy  = g.buffer_load(rdir_b, add(b3, ku(1)));
        const int rdz  = g.buffer_load(rdir_b, add(b3, ku(2)));
        const int cosw = fmax(add(add(mul(dx, rdx), mul(dy, rdy)), mul(dz, rdz)), f0); // max(0, d·rd)
        sir            = add(sir, mul(cosw, g.buffer_load(rrad_b, b3)));
        sig            = add(sig, mul(cosw, g.buffer_load(rrad_b, add(b3, ku(1)))));
        sib            = add(sib, mul(cosw, g.buffer_load(rrad_b, add(b3, ku(2)))));
        sw             = add(sw, cosw);
        const int dist = g.buffer_load(rdst_b, add(rbase1, ku(static_cast<crd::u32>(ray))));
        const int dw   = g.binary(KOp::Pow, cosw, kf(cfg.depth_sharpness)); // cos^sharpness (crisp)
        smean          = add(smean, mul(dw, dist));
        smean2         = add(smean2, mul(dw, mul(dist, dist)));
        sdw            = add(sdw, dw);
    }
    const int inv  = divv(kf(1.0), fmax(sw, kf(1.0e-8)));
    const int invd = divv(kf(1.0), fmax(sdw, kf(1.0e-8)));
    const int h    = kf(cfg.hysteresis);
    const auto lerp = [&](int prev, int nu) { return add(prev, mul(h, sub(nu, prev))); }; // prev + h·(new−prev)
    const int op3 = mul(idx, ku(3));
    const int op2 = mul(idx, ku(2));
    g.stmt_buffer_store(oirr_b, op3, lerp(g.buffer_load(pirr_b, op3), mul(sir, inv)));
    g.stmt_buffer_store(oirr_b, add(op3, ku(1)), lerp(g.buffer_load(pirr_b, add(op3, ku(1))), mul(sig, inv)));
    g.stmt_buffer_store(oirr_b, add(op3, ku(2)), lerp(g.buffer_load(pirr_b, add(op3, ku(2))), mul(sib, inv)));
    g.stmt_buffer_store(odpt_b, op2, lerp(g.buffer_load(pdpt_b, op2), mul(smean, invd)));
    g.stmt_buffer_store(odpt_b, add(op2, ku(1)), lerp(g.buffer_load(pdpt_b, add(op2, ku(1))), mul(smean2, invd)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// A test kernel: read N directions (F32 3/elem), encode→decode, write the recovered directions. Validates the octahedral
// round-trip end-to-end through the compute emitters. Buffers: 0=in_dir (F32 N·3), 1=out_dir (F32 N·3, out). local_size 64.
[[nodiscard]] inline KEntry build_ddgi_oct_roundtrip(KGraph& g, int n)
{
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    static_cast<void>(n);
    const int in_buf  = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_buf = g.buffer_decl(DType::F32, 0, 1, true);
    const int p       = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int p3      = mul(p, ku(3));

    const int mark = g.kernel_stmt_mark();
    const int dx = g.buffer_load(in_buf, p3);
    const int dy = g.buffer_load(in_buf, add(p3, ku(1)));
    const int dz = g.buffer_load(in_buf, add(p3, ku(2)));
    int ox = 0;
    int oy = 0;
    oct_encode(g, dx, dy, dz, ox, oy);
    int rx = 0;
    int ry = 0;
    int rz = 0;
    oct_decode(g, ox, oy, rx, ry, rz);
    g.stmt_buffer_store(out_buf, p3, rx);
    g.stmt_buffer_store(out_buf, add(p3, ku(1)), ry);
    g.stmt_buffer_store(out_buf, add(p3, ku(2)), rz);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::ddgi
