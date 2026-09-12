#pragma once

// ckir_svgf.hpp — D-007 B14-c: the SPATIOTEMPORAL DENOISER (SVGF / A-SVGF, Schied et al. 2017/18), authored in CKIR as a
// runnable statement-tier compute pass — bit-exact CPU oracle + both backends, NO ray-tracing dependency (it denoises
// whatever noisy radiance it is given; the real RT-traced source lands with B9). The heart of SVGF is the EDGE-STOPPING
// À-TROUS WAVELET filter: an increasing-stride 5×5 blur whose per-tap weights collapse across geometric edges (depth +
// normal) and across bright/dark luminance discontinuities (variance-guided), so a 1-spp noisy signal is smoothed WITHOUT
// bleeding across silhouettes. This first sub-slice (B14-c-1) is the à-trous iteration; temporal accumulation + moment/
// variance estimation + the A-SVGF gradient reset are the following sub-slices.
//
// Unlike the B13 resolve passes (which defer the neighborhood GATHER as a renderer leaf), a compute pass gathers its 5×5
// stencil from storage buffers directly — so the WHOLE filter (gather + weights + accumulate + normalize) is expressed and
// verified end-to-end. FP32 `precise` (no FMA) ⇒ every generic backend emitter lowers it and it bit-matches the oracle.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

// One à-trous edge-stopping iteration over a `width`×`height` image. `step` = the à-trous stride (1,2,4,8,16 across the 5
// iterations). σ_z/σ_n/σ_l tune the depth / normal / luminance edge-stopping falloff. The 5-tap separable base kernel is the
// SVGF {1/16, 1/4, 3/8, 1/4, 1/16}.
struct SvgfConfig
{
    int    width   = 32;
    int    height  = 32;
    int    step    = 1;
    double sigma_z = 1.0;
    double sigma_n = 128.0;
    double sigma_l = 4.0;
    // temporal integration (B14-c-2)
    double alpha_min    = 0.05; // floor on the color/moment blend weight ⇒ history length caps at 1/α_min (~20 frames)
    double depth_reject = 0.1;  // crd-lint-allow-untagged-physical: dimensionless disocclusion threshold (ratio on the depth metric, not a physical length) — |Δdepth| reject bound
    double normal_reject = 0.9; // n·n' disocclusion threshold (reject on a normal flip)
    // A-SVGF adaptive-α (B14-c-4): when the incoming sample is an OUTLIER from the history (|Δl| ≫ σ_hist ⇒ a real change,
    // not noise), boost α toward 1 ⇒ the accumulation RESETS ⇒ no lag/ghosting on fast lighting/motion change.
    bool   asvgf    = false; // enable the temporal-gradient adaptive α (Schied 2018 spirit, variance-aware)
    double asvgf_lo = 2.0;   // below this many σ_hist the change reads as noise (λ=0, keep accumulating)
    double asvgf_hi = 8.0;   // above this many σ_hist it reads as a real change (λ=1, full reset)

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0 && step > 0 && (width * height) % 64 == 0; }
};

namespace svgf_detail
{
constexpr double kEps      = 1.0e-6;
constexpr double kHker[5]  = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0}; // separable à-trous base (Schied)
constexpr double kGauss3[3] = {0.25, 0.5, 0.25}; // separable 3×3 gaussian for the variance pre-blur (luminance φ)
} // namespace svgf_detail

// Build ONE GOLD-STANDARD à-trous iteration as a statement-tier compute kernel — the faithful Schied 2017 edge-stopping
// weights (NOT a simplified proxy). One thread per pixel (local_size 64, grid = W·H/64). Buffers: 0=color_in (F32 W·H·3
// interleaved rgb), 1=gbuf (F32 W·H·4 = depth,nx,ny,nz), 2=var_in (F32 W·H), 3=color_out (F32 W·H·3, out), 4=var_out (F32
// W·H, out). Deterministic (fixed ascending tap order, no atomics). Weights w = h·w_z·w_n·w_l:
//   w_z = exp(−|z_p−z_q| / (σ_z·|∇z_p·(p−q)| + ε))   — DEPTH-GRADIENT aware (∇z from unit-stride neighbours; edges hold under
//        oblique surfaces, the whole point vs a flat |Δz|),
//   w_n = max(0, n_p·n_q)^σ_n,
//   w_l = exp(−|l_p−l_q| / (σ_l·√(g₃ₓ₃(Var_p)) + ε))  — luminance guided by a 3×3-GAUSSIAN-blurred variance (kills the
//        single-pixel-variance noise that would otherwise let fireflies survive).
// Color out = Σ(h·w)·c / Σ(h·w); variance out = Σ(h·w)²·Var_q / (Σ h·w)² (the variance of the weighted mean — Schied §4.2).
[[nodiscard]] inline KEntry build_svgf_atrous(KGraph& g, const SvgfConfig& cfg)
{
    const int   wd  = cfg.width;
    const int   ht  = cfg.height;
    const int   st  = cfg.step;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  kf  = [&](crd::f64 v) { return g.constant(v, sh1, DType::F32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int col_in  = g.buffer_decl(DType::F32, 0, 0, false);
    const int gbuf    = g.buffer_decl(DType::F32, 0, 1, false);
    const int var_in  = g.buffer_decl(DType::F32, 0, 2, false);
    const int col_out = g.buffer_decl(DType::F32, 0, 3, true);
    const int var_out = g.buffer_decl(DType::F32, 0, 4, true);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int fzero   = kf(0.0);

    const int mark = g.kernel_stmt_mark();
    const int p    = add(mul(wid, ku(64)), tid); // flat pixel index
    const int x    = g.binary(KOp::Mod, p, ku(static_cast<crd::u32>(wd)));
    const int y    = g.binary(KOp::Div, p, ku(static_cast<crd::u32>(wd)));

    // center G-buffer + luminance
    const auto lum = [&](int base) {
        return add(add(mul(g.buffer_load(col_in, add(base, ku(0))), kf(0.2126)), mul(g.buffer_load(col_in, add(base, ku(1))), kf(0.7152))),
                   mul(g.buffer_load(col_in, add(base, ku(2))), kf(0.0722)));
    };
    const int cp4  = mul(p, ku(4));
    const int cp3  = mul(p, ku(3));
    const int cz   = g.buffer_load(gbuf, add(cp4, ku(0)));
    const int cnx  = g.buffer_load(gbuf, add(cp4, ku(1)));
    const int cny  = g.buffer_load(gbuf, add(cp4, ku(2)));
    const int cnz  = g.buffer_load(gbuf, add(cp4, ku(3)));
    const int cl   = lum(cp3);

    // clamp a U32 coordinate by a compile-time offset (per-tap sign known ⇒ no unsigned underflow)
    const auto clampc = [&](int coord, int off, int dim) -> int {
        if (off == 0) { return coord; }
        if (off > 0) { return g.binary(KOp::Min, add(coord, ku(static_cast<crd::u32>(off))), ku(static_cast<crd::u32>(dim - 1))); }
        const int a = -off;
        return g.select(g.binary(KOp::CmpGe, coord, ku(static_cast<crd::u32>(a))), sub(coord, ku(static_cast<crd::u32>(a))), ku(0));
    };
    const auto pix = [&](int px, int py) { return add(mul(py, ku(static_cast<crd::u32>(wd))), px); }; // flat index of (px,py)

    // ∇z_p — screen-space depth gradient from unit-stride neighbours (dz/dx, dz/dy)
    const int dzdx = sub(g.buffer_load(gbuf, mul(pix(clampc(x, 1, wd), y), ku(4))), cz);
    const int dzdy = sub(g.buffer_load(gbuf, mul(pix(x, clampc(y, 1, ht)), ku(4))), cz);

    // g₃ₓ₃(Var_p) — 3×3-gaussian-blurred center variance → the luminance edge denominator φ_l
    int gvar = fzero;
    for (int j = -1; j <= 1; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            const int   bq = pix(clampc(x, i, wd), clampc(y, j, ht));
            const double gk = svgf_detail::kGauss3[i + 1] * svgf_detail::kGauss3[j + 1];
            gvar            = add(gvar, mul(kf(gk), g.buffer_load(var_in, bq)));
        }
    }
    const int lden = add(mul(kf(cfg.sigma_l), g.unary(KOp::Sqrt, g.binary(KOp::Max, gvar, fzero))), kf(svgf_detail::kEps)); // σ_l·√(g₃ₓ₃Var)+ε

    int sr   = fzero;
    int sg   = fzero;
    int sb   = fzero;
    int sw   = fzero;
    int svar = fzero;
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int qx = clampc(x, dx * st, wd);
            const int qy = clampc(y, dy * st, ht);
            const int q  = add(mul(qy, ku(static_cast<crd::u32>(wd))), qx);
            const int q4 = mul(q, ku(4));
            const int q3 = mul(q, ku(3));
            const int qz = g.buffer_load(gbuf, add(q4, ku(0)));
            const int qnx = g.buffer_load(gbuf, add(q4, ku(1)));
            const int qny = g.buffer_load(gbuf, add(q4, ku(2)));
            const int qnz = g.buffer_load(gbuf, add(q4, ku(3)));
            const int qr  = g.buffer_load(col_in, add(q3, ku(0)));
            const int qg  = g.buffer_load(col_in, add(q3, ku(1)));
            const int qb  = g.buffer_load(col_in, add(q3, ku(2)));
            const int ql  = lum(q3);
            const int qvar = g.buffer_load(var_in, q);

            // w_z = exp(-|cz-qz| / (σ_z·|∇z·Δp| + ε)); ∇z·Δp = dzdx·(dx·st) + dzdy·(dy·st) (compile-time tap offsets)
            const int proj  = add(mul(dzdx, kf(static_cast<double>(dx * st))), mul(dzdy, kf(static_cast<double>(dy * st))));
            const int phi_z = add(mul(kf(cfg.sigma_z), g.unary(KOp::Abs, proj)), kf(svgf_detail::kEps));
            const int wz    = g.unary(KOp::Exp, g.binary(KOp::Sub, fzero, g.binary(KOp::Div, g.unary(KOp::Abs, sub(cz, qz)), phi_z)));
            // w_n = pow(max(0, n·n'), σ_n)
            const int ndot = add(add(mul(cnx, qnx), mul(cny, qny)), mul(cnz, qnz));
            const int wn   = g.binary(KOp::Pow, g.binary(KOp::Max, ndot, fzero), kf(cfg.sigma_n));
            // w_l = exp(-|cl-ql| / lden)
            const int wl = g.unary(KOp::Exp, g.binary(KOp::Sub, fzero, g.binary(KOp::Div, g.unary(KOp::Abs, sub(cl, ql)), lden)));

            const double h = svgf_detail::kHker[dy + 2] * svgf_detail::kHker[dx + 2];
            const int    w = mul(mul(kf(h), wz), mul(wn, wl));
            sr             = add(sr, mul(w, qr));
            sg             = add(sg, mul(w, qg));
            sb             = add(sb, mul(w, qb));
            sw             = add(sw, w);
            svar           = add(svar, mul(mul(w, w), qvar));
        }
    }
    g.stmt_buffer_store(col_out, add(cp3, ku(0)), g.binary(KOp::Div, sr, sw));
    g.stmt_buffer_store(col_out, add(cp3, ku(1)), g.binary(KOp::Div, sg, sw));
    g.stmt_buffer_store(col_out, add(cp3, ku(2)), g.binary(KOp::Div, sb, sw));
    g.stmt_buffer_store(var_out, p, g.binary(KOp::Div, svar, mul(sw, sw)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the SVGF TEMPORAL INTEGRATION pass (B14-c-2) — the exponential-history accumulator that makes the denoiser stable
// across frames + estimates per-pixel variance from luminance MOMENTS (the variance the à-trous is guided by). One thread
// per pixel. Reproject the previous frame along the motion vector (nearest tap), REJECT the history on disocclusion (depth
// jump OR normal flip OR off-screen) by forcing α=1 (⇒ the reprojected sample is discarded, no ghost), else blend with
// α = max(1/hist, α_min). Accumulate m1=E[l], m2=E[l²] the same way ⇒ Var = max(m2−m1², 0). Deterministic (no atomics).
// Buffers: 0=cur_color(F32 W·H·3), 1=cur_gbuf(F32 W·H·4 depth,n), 2=motion(F32 W·H·2 = Δpx to the PREV position),
// 3=prev_color(F32 W·H·3), 4=prev_stat(F32 W·H·4 = m1,m2,hist,var), 5=prev_gbuf(F32 W·H·4), 6=out_color(F32 W·H·3, out),
// 7=out_stat(F32 W·H·4 = m1,m2,hist,var, out).
[[nodiscard]] inline KEntry build_svgf_temporal(KGraph& g, const SvgfConfig& cfg)
{
    const int   wd  = cfg.width;
    const int   ht  = cfg.height;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  kf  = [&](crd::f64 v) { return g.constant(v, sh1, DType::F32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto  divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto  fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto  fmin = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto  lerp = [&](int a, int b, int t) { return add(a, mul(t, sub(b, a))); }; // a + t·(b−a) (Mix not in compute emitter)

    const int cur_col  = g.buffer_decl(DType::F32, 0, 0, false);
    const int cur_gb   = g.buffer_decl(DType::F32, 0, 1, false);
    const int motion   = g.buffer_decl(DType::F32, 0, 2, false);
    const int prev_col = g.buffer_decl(DType::F32, 0, 3, false);
    const int prev_st  = g.buffer_decl(DType::F32, 0, 4, false);
    const int prev_gb  = g.buffer_decl(DType::F32, 0, 5, false);
    const int out_col  = g.buffer_decl(DType::F32, 0, 6, true);
    const int out_st   = g.buffer_decl(DType::F32, 0, 7, true);
    const int tid      = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid      = g.builtin(KBuiltin::WorkgroupIndex);
    const int f0       = kf(0.0);
    const int f1       = kf(1.0);

    const int mark = g.kernel_stmt_mark();
    const int p    = add(mul(wid, ku(64)), tid);
    const int x    = g.binary(KOp::Mod, p, ku(static_cast<crd::u32>(wd)));
    const int y    = g.binary(KOp::Div, p, ku(static_cast<crd::u32>(wd)));
    const int fx   = g.cast(x, DType::F32);
    const int fy   = g.cast(y, DType::F32);

    // reproject to the previous position (nearest); clamp for a safe load, but track true in-bounds for the reject.
    const int fpx  = add(fx, g.buffer_load(motion, mul(p, ku(2))));
    const int fpy  = add(fy, g.buffer_load(motion, add(mul(p, ku(2)), ku(1))));
    const int ppxf = g.unary(KOp::Floor, add(fpx, kf(0.5)));
    const int ppyf = g.unary(KOp::Floor, add(fpy, kf(0.5)));
    const int okx  = mul(g.select(g.binary(KOp::CmpGe, ppxf, f0), f1, f0), g.select(g.binary(KOp::CmpLt, ppxf, kf(wd)), f1, f0));
    const int oky  = mul(g.select(g.binary(KOp::CmpGe, ppyf, f0), f1, f0), g.select(g.binary(KOp::CmpLt, ppyf, kf(ht)), f1, f0));
    const int ppxu = g.cast(fmax(fmin(ppxf, kf(wd - 1)), f0), DType::U32);
    const int ppyu = g.cast(fmax(fmin(ppyf, kf(ht - 1)), f0), DType::U32);
    const int pp   = add(mul(ppyu, ku(static_cast<crd::u32>(wd))), ppxu);
    const int pp4  = mul(pp, ku(4));
    const int pp3  = mul(pp, ku(3));

    // disocclusion: reject on depth jump, normal flip, or off-screen
    const int cp4  = mul(p, ku(4));
    const int cz   = g.buffer_load(cur_gb, cp4);
    const int pz   = g.buffer_load(prev_gb, pp4);
    const int dok  = g.select(g.binary(KOp::CmpLt, g.unary(KOp::Abs, sub(cz, pz)), kf(cfg.depth_reject)), f1, f0);
    const int ndot = add(add(mul(g.buffer_load(cur_gb, add(cp4, ku(1))), g.buffer_load(prev_gb, add(pp4, ku(1)))),
                             mul(g.buffer_load(cur_gb, add(cp4, ku(2))), g.buffer_load(prev_gb, add(pp4, ku(2))))),
                         mul(g.buffer_load(cur_gb, add(cp4, ku(3))), g.buffer_load(prev_gb, add(pp4, ku(3)))));
    const int nok   = g.select(g.binary(KOp::CmpGt, ndot, kf(cfg.normal_reject)), f1, f0);
    const int valid = g.binary(KOp::CmpGt, mul(mul(okx, oky), mul(dok, nok)), kf(0.5)); // bool

    // current luminance
    const int cp3 = mul(p, ku(3));
    const int cl  = add(add(mul(g.buffer_load(cur_col, cp3), kf(0.2126)), mul(g.buffer_load(cur_col, add(cp3, ku(1))), kf(0.7152))),
                        mul(g.buffer_load(cur_col, add(cp3, ku(2))), kf(0.0722)));

    // history + blend weight (invalid ⇒ hist=1 ⇒ α=1 ⇒ lerp discards the reprojected sample = no ghost)
    const int hist       = g.select(valid, add(g.buffer_load(prev_st, add(pp4, ku(2))), f1), f1);
    const int base_alpha = fmax(divv(f1, hist), kf(cfg.alpha_min));
    int       alpha      = base_alpha;
    if (cfg.asvgf)
    {
        // λ = clamp((|Δl|/σ_hist − lo) / (hi − lo), 0, 1); α = lerp(base, 1, λ). Invalid history ⇒ base=1 already ⇒ α=1.
        const int delta = g.unary(KOp::Abs, sub(cl, g.buffer_load(prev_st, pp4)));         // |cur luma − history mean|
        const int sigma = g.unary(KOp::Sqrt, fmax(g.buffer_load(prev_st, add(pp4, ku(3))), kf(1.0e-8))); // √Var_hist
        const int ratio = divv(delta, add(sigma, kf(1.0e-4)));
        const int lam   = fmin(fmax(divv(sub(ratio, kf(cfg.asvgf_lo)), kf(cfg.asvgf_hi - cfg.asvgf_lo)), f0), f1);
        alpha           = add(base_alpha, mul(lam, sub(f1, base_alpha))); // lerp(base_alpha, 1, λ)
    }

    for (int c = 0; c < 3; ++c)
    {
        g.stmt_buffer_store(out_col, add(cp3, ku(static_cast<crd::u32>(c))),
                            lerp(g.buffer_load(prev_col, add(pp3, ku(static_cast<crd::u32>(c)))), g.buffer_load(cur_col, add(cp3, ku(static_cast<crd::u32>(c)))), alpha));
    }
    const int m1  = lerp(g.buffer_load(prev_st, pp4), cl, alpha);
    const int m2  = lerp(g.buffer_load(prev_st, add(pp4, ku(1))), mul(cl, cl), alpha);
    const int var = fmax(sub(m2, mul(m1, m1)), f0);
    g.stmt_buffer_store(out_st, cp4, m1);
    g.stmt_buffer_store(out_st, add(cp4, ku(1)), m2);
    g.stmt_buffer_store(out_st, add(cp4, ku(2)), hist);
    g.stmt_buffer_store(out_st, add(cp4, ku(3)), var);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir
