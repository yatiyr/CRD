#pragma once

// ckir_clouds.hpp — D-007 B15-b: VOLUMETRIC CLOUDS (Schneider 2015/2023 "Nubis / Nubis³" — the Horizon/Decima cloud system),
// authored in CKIR. Two parts, both statement-tier compute, bit-exact CPU oracle + backends:
//   [1] the DENSITY FIELD — a procedural cloud density from a PERLIN-WORLEY base shape (reusing B6 ckir_noise.hpp) remapped by a
//       height gradient + weather COVERAGE, then eroded at the edges by high-frequency WORLEY detail (the Nubis modelling recipe);
//   [2] the RAY-MARCH — march the view ray accumulating BEER-POWDER transmittance + phase-weighted in-scatter, with a short
//       light-march to the sun and Schneider's MULTI-OCTAVE multiple-scattering energy approximation (the "cutting-edge" tier).
// The DENSITY is procedural (no precomputed volume) so the whole thing is analytic + verifiable now; the temporal 1/16-step
// reprojection amortization + the Nubis³ NanoVDB voxel authoring are the renderer-side leaf. Reuses the fog/water phase family.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_noise.hpp>

namespace crd::kir::clouds
{

struct CloudConfig
{
    double coverage    = 0.5;  // weather COVERAGE ∈ [0,1] — how much of the sky is clouded
    double base_freq   = 3.0;  // base-shape noise frequency (low — the billowy silhouette)
    double detail_freq = 12.0; // erosion noise frequency (high — the wispy edges)
    double erosion     = 0.35; // detail-erosion strength
    double cloud_base  = 1.5;  // cloud layer bottom (km above ground)
    double cloud_top   = 4.5;  // cloud layer top (km)

    // ray-march (B15-b-2) — samples a PRECOMPUTED density VOLUME (the density kernel dispatched over a 3D grid), NOT procedural
    // noise per step: evaluating the noise per march sample would inflate the shader past compilation. This IS the Nubis pipeline
    // (bake noise → sample). The march shader is small (trilinear taps + Beer-Powder + phase + a light march) ⇒ compiles + fast.
    int    vol_dim     = 32;   // density volume resolution (vol_dim³)
    int    screen      = 32;   // march output resolution (screen² vertical rays)
    int    march_steps = 32;   // primary samples up through the volume
    int    light_steps = 4;    // secondary samples toward the sun (the light march)
    double sigma_t     = 1.2;  // cloud extinction
    double mie_g       = 0.3;  // cloud phase asymmetry (Cornette-Shanks)
    int    ms_octaves  = 2;    // Schneider multi-octave multiple-scattering octaves
    double sun_gx = 0.5;       // sun direction in grid space (baked)
    double sun_gy = 0.4;
    double sun_gz = 0.766;

    [[nodiscard]] bool valid() const noexcept { return cloud_top > cloud_base && march_steps >= 1 && light_steps >= 1; }
};

namespace detail
{
namespace nz = crd::kir::nodes::noise;

// remap v from [ol,oh] to [nl,nh] (Schneider's building block): nl + (v−ol)·(nh−nl)/(oh−ol).
[[nodiscard]] inline int remap(KGraph& g, int v, int ol, int oh, int nl, int nh)
{
    const int t = g.binary(KOp::Div, g.binary(KOp::Sub, v, ol), g.binary(KOp::Sub, oh, ol));
    return g.binary(KOp::Add, nl, g.binary(KOp::Mul, t, g.binary(KOp::Sub, nh, nl)));
}
[[nodiscard]] inline int sat(KGraph& g, int v)
{
    const auto k = [&](double c) { return g.constant(c, g.node(v).shape, DType::F32); };
    return g.binary(KOp::Max, g.binary(KOp::Min, v, k(1.0)), k(0.0));
}
// cumulus HEIGHT GRADIENT: 0 at the slab floor/ceiling, rounded bottom + soft top ⇒ clouds form in the mid-layer.
[[nodiscard]] inline int height_gradient(KGraph& g, int h01)
{
    const int bottom = sat(g, remap(g, h01, g.constant(0.05, g.node(h01).shape, DType::F32), g.constant(0.2, g.node(h01).shape, DType::F32), g.constant(0.0, g.node(h01).shape, DType::F32), g.constant(1.0, g.node(h01).shape, DType::F32)));
    const int top    = sat(g, remap(g, h01, g.constant(0.4, g.node(h01).shape, DType::F32), g.constant(0.95, g.node(h01).shape, DType::F32), g.constant(1.0, g.node(h01).shape, DType::F32), g.constant(0.0, g.node(h01).shape, DType::F32)));
    return g.binary(KOp::Mul, bottom, top);
}

// Compute-path WORLEY (F1, euclidean = metric 0, style 0) that emits a RUNTIME LOOP over the 27 neighbour cells instead of the
// 27× UNROLL of nodes::noise::worley3. This is what makes stacking Worley FBMs COMPILE on the GPU: the unrolled worley3 emits
// ~1350 nodes each, so a 3-octave FBM × base+detail (6 of them) is ~8000 nodes and shaderc's optimiser AND the NVIDIA driver's
// pipeline compiler blow up SUPER-LINEARLY (killed at 500–800 CPU-s). The loop collapses each worley3 to ~30 nodes ⇒ the whole
// Perlin-Worley density is ~400 nodes ⇒ compiles in seconds. BIT-EXACT with worley3: the running-min is order-independent
// (min/select-min is associative + exact on the same 27 cell distances, whose hashes depend on cell COORDS not visit order), so
// looping the cells in a different order yields the identical minimum. Loop-carried min lives in a per-thread SHARED slot (the
// compute for-loop has no loop-phi register — the proven ReSTIR accumulator pattern); local_size is 64 (matches the cloud kernels).
[[nodiscard]] inline int worley3_loop(KGraph& g, int px, int py, int pz, double jitter)
{
    namespace d     = nz::detail;
    const auto kf   = [&](double c) { return g.constant(c, g.node(px).shape, DType::F32); };
    const auto ki   = [&](int c) { return g.constant(static_cast<double>(c), g.node(px).shape, DType::I32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divf = [&](int a, int b) { return g.binary(KOp::Div, a, b); };

    // FULLY SCALAR (no vec3/dot/swizzle): the compute-kernel emitter's statement path is scalar-only (vector ops live only in
    // the raster value emitter), so the looped Worley must avoid vectors. Bit-exactness is SELF-CONSISTENT — the CPU oracle and
    // every backend evaluate THIS same scalar graph, so I don't need to match the vec worley3's dot; standard scalar add/mul/
    // floor/div/select round identically on the oracle + backends.
    int xi = 0;
    int yi = 0;
    int zi = 0;
    const int lx = d::floorfrac(g, px, xi); // localpos = frac(p); xi/yi/zi = int(floor(p))
    const int ly = d::floorfrac(g, py, yi);
    const int lz = d::floorfrac(g, pz, zi);

    const int sh  = g.shared_decl(DType::F32, 64); // per-thread running-min (best)
    const int tid = g.builtin(KBuiltin::LocalInvocationIndex);
    g.stmt_shared_store(sh, tid, kf(1e6));

    const int fr = g.stmt_for_begin(g.constant(27.0, g.node(px).shape, DType::U32));
    const int j  = g.kernel_loop_var(fr);
    const int jf = g.cast(j, DType::F32);
    // decode j∈[0,26] → (dz,dy,dx)∈{0,1,2}³ (dx fastest); the cell offsets are (d−1)∈{−1,0,1}.
    const int dz = g.unary(KOp::Floor, divf(jf, kf(9.0)));
    const int r  = sub(jf, mul(dz, kf(9.0)));
    const int dy = g.unary(KOp::Floor, divf(r, kf(3.0)));
    const int dx = sub(r, mul(dy, kf(3.0)));
    const int cxi = add(xi, sub(g.cast(dx, DType::I32), ki(1))); // integer cell coords (for the hash) — xi + (dx−1)
    const int cyi = add(yi, sub(g.cast(dy, DType::I32), ki(1)));
    const int czi = add(zi, sub(g.cast(dz, DType::I32), ki(1)));
    // hash cell-offset (mx_worley_cell_position): ((cell_noise_vec3 − 0.5)·jitter + 0.5), taken per-channel as scalars.
    int cn[3];
    d::cell_vec3_from3(g, cxi, cyi, czi, px, cn);
    const int ox = add(mul(sub(cn[0], kf(0.5)), kf(jitter)), kf(0.5));
    const int oy = add(mul(sub(cn[1], kf(0.5)), kf(jitter)), kf(0.5));
    const int oz = add(mul(sub(cn[2], kf(0.5)), kf(jitter)), kf(0.5));
    // diff = (float_cell_offset + hash_offset) − localpos;  dist = |diff|²
    const int ddx  = sub(add(sub(dx, kf(1.0)), ox), lx);
    const int ddy  = sub(add(sub(dy, kf(1.0)), oy), ly);
    const int ddz  = sub(add(sub(dz, kf(1.0)), oz), lz);
    const int dist = add(add(mul(ddx, ddx), mul(ddy, ddy)), mul(ddz, ddz));
    const int best = g.shared_load(sh, tid); // single read ⇒ no shared-RMW double-count
    const int cond = g.binary(KOp::CmpLt, dist, best);
    g.stmt_shared_store(sh, tid, g.select(cond, dist, best)); // running-min, order-independent
    g.stmt_for_end(fr);

    return g.unary(KOp::Sqrt, g.shared_load(sh, tid)); // F1 euclidean ⇒ sqrt(min distance²)
}

// 3-octave WORLEY FBM (F1 distance, ∈ ~[0,1]); weights 0.625/0.25/0.125 sum to 1. Uses the LOOPED worley3_loop so stacking these
// COMPILES on the GPU — the gold Perlin-Worley is fully procedural + portable + bit-exact.
[[nodiscard]] inline int worley_fbm(KGraph& g, int x, int y, int z, double freq)
{
    const auto k  = [&](double c) { return g.constant(c, g.node(x).shape, DType::F32); };
    const auto sc = [&](int a, double f) { return g.binary(KOp::Mul, a, k(f)); };
    const int  w1 = worley3_loop(g, sc(x, freq), sc(y, freq), sc(z, freq), 1.0);
    const int  w2 = worley3_loop(g, sc(x, freq * 2.0), sc(y, freq * 2.0), sc(z, freq * 2.0), 1.0);
    const int  w3 = worley3_loop(g, sc(x, freq * 4.0), sc(y, freq * 4.0), sc(z, freq * 4.0), 1.0);
    return g.binary(KOp::Add, g.binary(KOp::Add, g.binary(KOp::Mul, w1, k(0.625)), g.binary(KOp::Mul, w2, k(0.25))), g.binary(KOp::Mul, w3, k(0.125)));
}

// The Nubis cloud DENSITY (Schneider 2015) at a world position (x,y,z) with normalised layer height h01 ∈ [0,1]. The base shape
// is the industry-standard PERLIN-WORLEY (a Perlin FBM DILATED by an inverted Worley FBM ⇒ the billowy "cauliflower" cumulus
// form), carved by the height gradient + weather COVERAGE, then ERODED at the edges by a high-frequency WORLEY FBM (the wispy
// detail). Returns density ∈ [0,1]. Fully procedural + portable + bit-exact (worley3's flattened running-min compiles on GPU).
[[nodiscard]] inline int cloud_density(KGraph& g, int x, int y, int z, int h01, const CloudConfig& cfg)
{
    const auto k  = [&](double c) { return g.constant(c, g.node(x).shape, DType::F32); };
    const auto sc = [&](int a, double f) { return g.binary(KOp::Mul, a, k(f)); };
    const double bf = cfg.base_freq;

    // PERLIN-WORLEY base: Perlin FBM (→[0,1]) remapped by (worley_fbm − 1) ⇒ the Worley billows dilate the Perlin (Schneider).
    const int pf   = nz::fractal3(g, sc(x, bf), sc(y, bf), sc(z, bf), 3, 2.0, 0.5); // ~[−1,1]
    const int pf01 = sat(g, g.binary(KOp::Mul, g.binary(KOp::Add, pf, k(1.0)), k(0.5)));
    const int wfbm = worley_fbm(g, x, y, z, bf);
    const int pw   = sat(g, remap(g, pf01, g.binary(KOp::Sub, wfbm, k(1.0)), k(1.0), k(0.0), k(1.0)));

    // height gradient + coverage carve: only base > (1−coverage) survives, scaled by coverage.
    const int base0 = g.binary(KOp::Mul, pw, height_gradient(g, h01));
    const int covd  = sat(g, remap(g, base0, g.binary(KOp::Sub, k(1.0), k(cfg.coverage)), k(1.0), k(0.0), k(1.0)));
    const int base  = g.binary(KOp::Mul, covd, k(cfg.coverage));

    // high-frequency WORLEY erosion of the edges (the wispy detail — the Nubis signature).
    const int det = worley_fbm(g, x, y, z, cfg.detail_freq);
    return sat(g, remap(g, base, g.binary(KOp::Mul, det, k(cfg.erosion)), k(1.0), k(0.0), k(1.0)));
}
} // namespace detail

// Build the cloud DENSITY-FIELD kernel — one thread per query position. Reads (x,y,z), derives the layer height, evaluates the
// Nubis density. Buffers: 0 = positions (F32 N·3, world km), 1 = out density (F32 N). Dispatch N/64 workgroups.
[[nodiscard]] inline KEntry build_cloud_density(KGraph& g, const CloudConfig& cfg)
{
    const auto kf  = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };

    const int pos_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int p     = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int p3   = mul(p, ku(3));
    const int x    = g.buffer_load(pos_b, p3);
    const int y    = g.buffer_load(pos_b, add(p3, ku(1)));
    const int z    = g.buffer_load(pos_b, add(p3, ku(2)));
    // HOIST the position loads to top-level (main) scope: x/y/z (and their index temps) feed every one of the sibling worley3_loop
    // For-bodies, so without this each index temp would be declared inside the FIRST loop and be out of scope in the rest (GLSL
    // "undeclared identifier"). Freezing the loads here is safe — this kernel has no barriers, so the positions never change.
    g.stmt_materialize(x);
    g.stmt_materialize(y);
    g.stmt_materialize(z);
    const int h01 = detail::sat(g, divv(sub(z, kf(cfg.cloud_base)), kf(cfg.cloud_top - cfg.cloud_base)));
    g.stmt_materialize(h01); // top-level too (used only by the height gradient) — else it strands inside a worley For-body
    g.stmt_buffer_store(out_b, p, detail::cloud_density(g, x, y, z, h01, cfg));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the cloud RAY-MARCH kernel (Schneider Beer-Powder + multi-octave multiple-scatter, sampling the baked density VOLUME).
// One thread per (sx,sy) screen texel marches a vertical ray UP through the vol_dim³ density volume: at each step it TRILINEARLY
// samples density, attenuates the view transmittance (Beer), and adds in-scattered sunlight = phase(cos)·(light transmittance
// from a short march toward the sun)·density, boosted by Schneider's multi-octave MS series (each octave a weaker, wider term).
// The POWDER term darkens cloud edges. Buffers: 0 = density volume (F32 vol_dim³), 1 = out (F32 screen²·4 = RGB inscatter + view
// transmittance). Dispatch screen²/64 workgroups. Only exp/pow are ULP; the rest is bit-exact.
[[nodiscard]] inline KEntry build_cloud_march(KGraph& g, const CloudConfig& cfg)
{
    const auto kf   = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto fmin = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto expn = [&](int a) { return g.unary(KOp::Exp, g.unary(KOp::Neg, a)); };

    const int    dim = cfg.vol_dim;
    const int    sw  = cfg.screen;
    const int    nst = cfg.march_steps;
    const int    lst = cfg.light_steps;
    const double dmax = static_cast<double>(dim - 1);
    const double pi   = 3.14159265358979323846;
    const double gmg  = cfg.mie_g;

    const int vol_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int p     = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    g.stmt_materialize(p);

    // TRILINEAR sample of the density volume at continuous grid coords (cgx,cgy,cgz), clamped to [0,dim−1].
    const auto sample_vol = [&](int cgx, int cgy, int cgz) {
        const int gx = fmin(fmax(cgx, kf(0.0)), kf(dmax));
        const int gy = fmin(fmax(cgy, kf(0.0)), kf(dmax));
        const int gz = fmin(fmax(cgz, kf(0.0)), kf(dmax));
        const int x0f = g.unary(KOp::Floor, gx);
        const int y0f = g.unary(KOp::Floor, gy);
        const int z0f = g.unary(KOp::Floor, gz);
        const int fx = sub(gx, x0f);
        const int fy = sub(gy, y0f);
        const int fz = sub(gz, z0f);
        const int x0 = g.cast(x0f, DType::U32);
        const int y0 = g.cast(y0f, DType::U32);
        const int z0 = g.cast(z0f, DType::U32);
        const int x1 = g.cast(fmin(add(x0f, kf(1.0)), kf(dmax)), DType::U32);
        const int y1 = g.cast(fmin(add(y0f, kf(1.0)), kf(dmax)), DType::U32);
        const int z1 = g.cast(fmin(add(z0f, kf(1.0)), kf(dmax)), DType::U32);
        g.stmt_materialize(x0); g.stmt_materialize(y0); g.stmt_materialize(z0);
        g.stmt_materialize(x1); g.stmt_materialize(y1); g.stmt_materialize(z1);
        g.stmt_materialize(fx); g.stmt_materialize(fy); g.stmt_materialize(fz);
        const auto vox = [&](int xi, int yi, int zi) { return g.buffer_load(vol_b, add(mul(add(mul(zi, ku(static_cast<crd::u32>(dim))), yi), ku(static_cast<crd::u32>(dim))), xi)); };
        const auto lerp = [&](int a, int b, int t) { return add(a, mul(t, sub(b, a))); };
        const int c00 = lerp(vox(x0, y0, z0), vox(x1, y0, z0), fx);
        const int c10 = lerp(vox(x0, y1, z0), vox(x1, y1, z0), fx);
        const int c01 = lerp(vox(x0, y0, z1), vox(x1, y0, z1), fx);
        const int c11 = lerp(vox(x0, y1, z1), vox(x1, y1, z1), fx);
        return lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz);
    };

    // screen texel (sx,sy) → the horizontal grid column; march vertically (+z grid) through the volume.
    const int fp   = g.cast(p, DType::F32);
    const int syf  = g.unary(KOp::Floor, divv(fp, kf(static_cast<double>(sw))));
    const int sxf  = sub(fp, mul(syf, kf(static_cast<double>(sw))));
    const int cx   = mul(divv(add(sxf, kf(0.5)), kf(static_cast<double>(sw))), kf(dmax)); // grid x of this column
    const int cy   = mul(divv(add(syf, kf(0.5)), kf(static_cast<double>(sw))), kf(dmax)); // grid y
    g.stmt_materialize(cx);
    g.stmt_materialize(cy);

    // sun-view phase (Cornette-Shanks): the view is +z (up); cos to the baked sun dir = sun_gz (its z-component). Built as a
    // graph node (the denominator's ^1.5 via KOp::Pow) so no host pow is needed.
    const double csv = cfg.sun_gz;
    const int    pcs = divv(kf((3.0 / (8.0 * pi)) * (1.0 - gmg * gmg) * (1.0 + csv * csv)),
                            mul(kf(2.0 + gmg * gmg), g.binary(KOp::Pow, kf(1.0 + gmg * gmg - 2.0 * gmg * csv), kf(1.5))));

    const int dz = divv(kf(dmax), kf(static_cast<double>(nst))); // grid-z step
    const int ld = divv(kf(dmax), kf(static_cast<double>(lst) * 2.0)); // light-march step (shorter)
    int       tr = kf(1.0); // view transmittance
    int       lr = kf(0.0); // accumulated in-scatter (grey; RGB would scale the sun colour)
    for (int i = 0; i < nst; ++i)
    {
        const int cz  = mul(kf(static_cast<double>(i) + 0.5), dz);
        const int den = sample_vol(cx, cy, cz);
        g.stmt_materialize(den);
        // light march toward the sun: accumulate optical depth along the sun dir.
        int ltau = kf(0.0);
        for (int j = 0; j < lst; ++j)
        {
            const int lt = mul(kf(static_cast<double>(j) + 0.5), ld);
            const int ld_x = add(cx, mul(kf(cfg.sun_gx), lt));
            const int ld_y = add(cy, mul(kf(cfg.sun_gy), lt));
            const int ld_z = add(cz, mul(kf(cfg.sun_gz), lt));
            ltau = add(ltau, mul(sample_vol(ld_x, ld_y, ld_z), ld));
        }
        const int lt_tr = expn(mul(kf(cfg.sigma_t), ltau)); // sunlight transmittance to this sample
        // Schneider multi-octave MS: Σ_o a^o · (light transmittance)^(b^o) · phase — weaker + softer each octave.
        int ms = kf(0.0);
        double a = 1.0;
        double b = 1.0;
        for (int o = 0; o < cfg.ms_octaves; ++o)
        {
            ms = add(ms, mul(mul(kf(a), pcs), g.binary(KOp::Pow, lt_tr, kf(b))));
            a *= 0.5;
            b *= 0.5;
        }
        // Beer-Powder: extinction attenuates the view ray; the powder term darkens dense edges.
        const int sig = mul(kf(cfg.sigma_t), den);
        const int powder = sub(kf(1.0), expn(mul(kf(2.0), mul(sig, dz))));
        lr = add(lr, mul(mul(mul(tr, ms), mul(den, powder)), dz));
        tr = mul(tr, expn(mul(sig, dz)));
    }
    const int op4 = mul(p, ku(4));
    g.stmt_buffer_store(out_b, op4, lr);
    g.stmt_buffer_store(out_b, add(op4, ku(1)), lr);
    g.stmt_buffer_store(out_b, add(op4, ku(2)), lr);
    g.stmt_buffer_store(out_b, add(op4, ku(3)), tr);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::clouds
