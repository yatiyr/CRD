#pragma once

// ckir_gsplat.hpp — D-007 B19: 3D GAUSSIAN SPLATTING, the forward rasteriser (radiance fields as a first-class
// primitive). Research dossier: docs/research/2026-07-21-3dgs-frontier.md.
//
// ⭐ 3DGS IS A SORT + OIT-COMPOSITE PROBLEM, and Cerid already built both halves. The forward render is three moves:
//    1. PROJECT each anisotropic 3D Gaussian to a 2D screen-space Gaussian (the EWA splat, Zwicker 2001) — THIS FILE.
//    2. DEPTH-SORT the projected Gaussians (our GPU radix/onesweep; host-side for the B19-a correctness core).
//    3. Per pixel, COMPOSITE the sorted Gaussians front-to-back (the `over` operator — B17's A-buffer) — THIS FILE.
//
// A 3D Gaussian carries: position μ(3), anisotropic covariance stored as scale s(3) + rotation quaternion q(4),
// opacity α(1), and colour as spherical-harmonic coefficients (view-dependent). B19-a uses SH degree 0 (a constant
// RGB per Gaussian) — higher SH degrees (view dependence) are a B19-a2 follow-on. Everything is authored in CKIR so
// it lowers to all five backends; the compute tier is SCALAR, so the maths is written component-wise (the same
// discipline the hair kernels use — the vec3 forms are the raster tier's).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair.hpp> // reuse the scalar detail helpers (add/mul/sub/dv/sq/safe_sqrt/kf)

namespace crd::kir::gsplat
{

namespace detail
{
using crd::kir::hair::detail::add;
using crd::kir::hair::detail::dv;
using crd::kir::hair::detail::kf;
using crd::kir::hair::detail::mul;
using crd::kir::hair::detail::safe_sqrt;
using crd::kir::hair::detail::sq;
using crd::kir::hair::detail::sub;

inline constexpr double kShC0 = 0.28209479177387814; // Y_0^0 — the SH degree-0 basis constant
} // namespace detail

// ── PROJECT: per-Gaussian preprocess. One thread per Gaussian. ────────────────────────────────────────────────────
//
// Buffers:
//   b0 gaussians  (F32, 14 / Gaussian): [μx μy μz · sx sy sz · qx qy qz qw · opacity · shR shG shB]
//   b1 camera     (F32, 20): [R00..R22 (row-major 3×3 view rotation) · tx ty tz · fx fy cx cy · near · imgW imgH]
//   b2 out        (F32, 12 / Gaussian): [meanx meany depth · conicA conicB conicC · radius · colR colG colB · opacityOut · valid]
//
// The maths (EWA splat):
//   view space p = R·μ + t;  screen mean = (fx·px/pz + cx, fy·py/pz + cy);  depth = pz.
//   world covariance Σ = (R_q·S)(R_q·S)ᵀ;  view covariance Σ_c = R·Σ·Rᵀ = (R·R_q·S)(R·R_q·S)ᵀ (combine the rotations).
//   2D covariance Σ′ = J·Σ_c·Jᵀ with J the affine Jacobian of the perspective projection at p; + a 0.3 low-pass on
//   the diagonal (the original 3DGS dilation keeping sub-pixel splats renderable — Mip-Splatting replaces this in
//   B19-b).  conic = Σ′⁻¹;  radius = ⌈3·√λ_max(Σ′)⌉.
struct GsplatProjectConfig
{
    double near_plane = 0.2;   // Gaussians with view-z below this are culled (behind / on the camera)
    double lowpass    = 0.3;   // the 3DGS diagonal dilation (B19-b Mip-Splatting supersedes this when `mip`)
    // ── B19-b MIP-SPLATTING (Yu et al., CVPR 2024) — alias-free. ─────────────────────────────────────────────────
    // The naïve dilation above ADDS 0.3 to the 2D covariance diagonal but does NOT rescale opacity, so a splat's
    // total contributed energy (∝ opacity·√det Σ′) CHANGES as it shrinks below a pixel — that IS the aliasing (small
    // splats over-contribute, large ones erode). Mip-Splatting instead:
    //   · 3D SMOOTHING FILTER — cap each Gaussian's max frequency to the sampling Nyquist by adding a small isotropic
    //     term to the 3D (view) covariance, sized by the pixel footprint at its depth (d/f). View-consistent.
    //   · 2D MIP FILTER — convolve the projected Gaussian with the pixel's integration footprint AND rescale opacity
    //     by √(det Σ′ / det Σ′_mip) so the TOTAL energy is preserved exactly. That rescale is the whole fix.
    bool   mip       = false;
    double smooth_3d = 0.2;   // 3D filter size, as a fraction of the pixel footprint (d/f) — the frequency cap
    double mip_2d    = 0.3;   // 2D Mip filter variance (the pixel integration footprint, px²)
    int    local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat_project_kernel(KGraph& g, const GsplatProjectConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int gauss_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int cam_b   = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b   = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid     = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int  mark = g.kernel_stmt_mark();
    const int  gb   = g.binary(KOp::Mul, tid, cu(14U));
    const auto gl   = [&](int k) {
        const int v = g.buffer_load(gauss_b, g.binary(KOp::Add, gb, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const auto cl = [&](int k) {
        const int v = g.buffer_load(cam_b, cu(static_cast<crd::u32>(k)));
        g.stmt_materialize(v);
        return v;
    };

    // ── camera ──
    const int r00 = cl(0);
    const int r01 = cl(1);
    const int r02 = cl(2);
    const int r10 = cl(3);
    const int r11 = cl(4);
    const int r12 = cl(5);
    const int r20 = cl(6);
    const int r21 = cl(7);
    const int r22 = cl(8);
    const int tx  = cl(9);
    const int ty  = cl(10);
    const int tz  = cl(11);
    const int fx  = cl(12);
    const int fy  = cl(13);
    const int cx  = cl(14);
    const int cy  = cl(15);

    // ── Gaussian ──
    const int mux  = gl(0);
    const int muy  = gl(1);
    const int muz  = gl(2);
    const int sx   = gl(3);
    const int sy   = gl(4);
    const int sz   = gl(5);
    const int qx   = gl(6);
    const int qy   = gl(7);
    const int qz   = gl(8);
    const int qw   = gl(9);
    const int opac = gl(10);
    const int shr  = gl(11);
    const int shg  = gl(12);
    const int shb  = gl(13);

    // ── view-space position p = R·μ + t ──
    const int vx = add(g, add(g, add(g, mul(g, r00, mux), mul(g, r01, muy)), mul(g, r02, muz)), tx);
    const int vy = add(g, add(g, add(g, mul(g, r10, mux), mul(g, r11, muy)), mul(g, r12, muz)), ty);
    const int vz = add(g, add(g, add(g, mul(g, r20, mux), mul(g, r21, muy)), mul(g, r22, muz)), tz);
    const int vzs = mx(vz, ks(1.0e-6)); // guard the divides for a culled (behind-camera) Gaussian; masked out at the end
    const int inv_vz = dv(g, ks(1.0), vzs);

    // ── screen mean ──
    const int mean_x = add(g, mul(g, fx, mul(g, vx, inv_vz)), cx);
    const int mean_y = add(g, mul(g, fy, mul(g, vy, inv_vz)), cy);

    // ── R_q: quaternion → 3×3 rotation (assumes a normalised quaternion, as 3DGS keeps it) ──
    const int q00 = sub(g, ks(1.0), mul(g, ks(2.0), add(g, sq(g, qy), sq(g, qz))));
    const int q01 = mul(g, ks(2.0), sub(g, mul(g, qx, qy), mul(g, qw, qz)));
    const int q02 = mul(g, ks(2.0), add(g, mul(g, qx, qz), mul(g, qw, qy)));
    const int q10 = mul(g, ks(2.0), add(g, mul(g, qx, qy), mul(g, qw, qz)));
    const int q11 = sub(g, ks(1.0), mul(g, ks(2.0), add(g, sq(g, qx), sq(g, qz))));
    const int q12 = mul(g, ks(2.0), sub(g, mul(g, qy, qz), mul(g, qw, qx)));
    const int q20 = mul(g, ks(2.0), sub(g, mul(g, qx, qz), mul(g, qw, qy)));
    const int q21 = mul(g, ks(2.0), add(g, mul(g, qy, qz), mul(g, qw, qx)));
    const int q22 = sub(g, ks(1.0), mul(g, ks(2.0), add(g, sq(g, qx), sq(g, qy))));

    // ── T = R·R_q  (the view rotation composed with the Gaussian rotation) ──
    const auto m3 = [&](int a0, int a1, int a2, int b0, int b1, int b2) { return add(g, add(g, mul(g, a0, b0), mul(g, a1, b1)), mul(g, a2, b2)); };
    const int t00 = m3(r00, r01, r02, q00, q10, q20);
    const int t01 = m3(r00, r01, r02, q01, q11, q21);
    const int t02 = m3(r00, r01, r02, q02, q12, q22);
    const int t10 = m3(r10, r11, r12, q00, q10, q20);
    const int t11 = m3(r10, r11, r12, q01, q11, q21);
    const int t12 = m3(r10, r11, r12, q02, q12, q22);
    const int t20 = m3(r20, r21, r22, q00, q10, q20);
    const int t21 = m3(r20, r21, r22, q01, q11, q21);
    const int t22 = m3(r20, r21, r22, q02, q12, q22);

    // ── M = T·S  (scale the columns), then Σ_c = M·Mᵀ (view-space covariance, 6 uniques) ──
    const int m00 = mul(g, t00, sx);
    const int m01 = mul(g, t01, sy);
    const int m02 = mul(g, t02, sz);
    const int m10 = mul(g, t10, sx);
    const int m11 = mul(g, t11, sy);
    const int m12 = mul(g, t12, sz);
    const int m20 = mul(g, t20, sx);
    const int m21 = mul(g, t21, sy);
    const int m22 = mul(g, t22, sz);
    const int c00 = add(g, add(g, sq(g, m00), sq(g, m01)), sq(g, m02));
    const int c01 = add(g, add(g, mul(g, m00, m10), mul(g, m01, m11)), mul(g, m02, m12));
    const int c02 = add(g, add(g, mul(g, m00, m20), mul(g, m01, m21)), mul(g, m02, m22));
    const int c11 = add(g, add(g, sq(g, m10), sq(g, m11)), sq(g, m12));
    const int c12 = add(g, add(g, mul(g, m10, m20), mul(g, m11, m21)), mul(g, m12, m22));
    const int c22 = add(g, add(g, sq(g, m20), sq(g, m21)), sq(g, m22));

    // ── B19-b: 3D SMOOTHING FILTER (Mip-Splatting). Add the depth-scaled pixel footprint to the 3D covariance
    //    diagonal so the projected Gaussian can never exceed the sampling Nyquist. Applied in view space here (the
    //    single-view render approximation of the paper's per-Gaussian world-space filter, which training precomputes
    //    from the minimal observed distance across all views). Isotropic ⇒ diagonal only. ──
    int cs00 = c00;
    int cs11 = c11;
    int cs22 = c22;
    if (cfg.mip)
    {
        const int s3  = mul(g, ks(cfg.smooth_3d), mul(g, vz, dv(g, ks(1.0), fx))); // world size of a pixel at depth × scale
        const int s3q = sq(g, s3);
        cs00 = add(g, c00, s3q);
        cs11 = add(g, c11, s3q);
        cs22 = add(g, c22, s3q);
    }

    // ── Jacobian J of the perspective projection at p (2×3, rows (j00,0,j02),(0,j11,j12)) ──
    const int j00 = mul(g, fx, inv_vz);
    const int j02 = g.unary(KOp::Neg, mul(g, fx, mul(g, vx, sq(g, inv_vz))));
    const int j11 = mul(g, fy, inv_vz);
    const int j12 = g.unary(KOp::Neg, mul(g, fy, mul(g, vy, sq(g, inv_vz))));

    // ── Σ′ = J·Σ_c·Jᵀ  (2×2 symmetric: a,b,c) ── first Mj = J·Σ_c (2×3), then Σ′ = Mj·Jᵀ ──
    const int mj00 = add(g, mul(g, j00, cs00), mul(g, j02, c02));
    const int mj01 = add(g, mul(g, j00, c01), mul(g, j02, c12));
    const int mj02 = add(g, mul(g, j00, c02), mul(g, j02, cs22));
    const int mj11 = add(g, mul(g, j11, cs11), mul(g, j12, c12));
    const int mj12 = add(g, mul(g, j11, c12), mul(g, j12, c22));
    int cov_a = add(g, mul(g, mj00, j00), mul(g, mj02, j02));
    int cov_b = add(g, mul(g, mj01, j11), mul(g, mj02, j12));
    int cov_c = add(g, mul(g, mj11, j11), mul(g, mj12, j12));
    int opac_out = opac;
    if (cfg.mip)
    {
        // ── B19-b: 2D MIP FILTER, the alias-free core. Convolve with the pixel footprint (add mip_2d to the diagonal)
        //    AND rescale opacity by √(det Σ′ / det Σ′_mip) so the TOTAL energy (∝ opacity·√det) is preserved EXACTLY.
        //    That rescale is the entire difference from the naïve dilation: a splat shrinking below a pixel now loses
        //    opacity as it should, instead of over-contributing — which is what removes the aliasing on zoom.
        const int det0 = mx(sub(g, mul(g, cov_a, cov_c), sq(g, cov_b)), ks(1.0e-12));
        cov_a = add(g, cov_a, ks(cfg.mip_2d));
        cov_c = add(g, cov_c, ks(cfg.mip_2d));
        const int det1 = mx(sub(g, mul(g, cov_a, cov_c), sq(g, cov_b)), ks(1.0e-12));
        opac_out = mul(g, opac, safe_sqrt(g, dv(g, det0, det1)));
    }
    else
    {
        // the original 3DGS low-pass: keep every splat at least ~1px, NO energy rescale ⇒ aliases on scale change.
        cov_a = add(g, cov_a, ks(cfg.lowpass));
        cov_c = add(g, cov_c, ks(cfg.lowpass));
    }

    // ── conic = Σ′⁻¹ and radius = ⌈3√λ_max⌉ ──
    const int det  = sub(g, mul(g, cov_a, cov_c), sq(g, cov_b));
    const int deti = dv(g, ks(1.0), mx(det, ks(1.0e-12)));
    const int con_a = mul(g, cov_c, deti);
    const int con_b = mul(g, g.unary(KOp::Neg, cov_b), deti);
    const int con_c = mul(g, cov_a, deti);
    const int mid  = mul(g, ks(0.5), add(g, cov_a, cov_c));
    const int disc = safe_sqrt(g, sub(g, sq(g, mid), det));
    const int lmax = add(g, mid, disc);
    const int radius = g.unary(KOp::Ceil, mul(g, ks(3.0), safe_sqrt(g, lmax)));

    // ── SH degree-0 colour (view-independent): c = 0.5 + C0·sh, clamped ≥ 0 ──
    const int col_r = mx(add(g, ks(0.5), mul(g, ks(kShC0), shr)), ks(0.0));
    const int col_g = mx(add(g, ks(0.5), mul(g, ks(kShC0), shg)), ks(0.0));
    const int col_b = mx(add(g, ks(0.5), mul(g, ks(kShC0), shb)), ks(0.0));

    // valid iff in front of the near plane (and a finite footprint)
    const int valid = g.select(g.binary(KOp::CmpGt, vz, ks(cfg.near_plane)), ks(1.0), ks(0.0));

    const int ob = g.binary(KOp::Mul, tid, cu(12U));
    const auto st = [&](int k, int v) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(static_cast<crd::u32>(k))), v); };
    st(0, mean_x); st(1, mean_y); st(2, vz);
    st(3, con_a); st(4, con_b); st(5, con_c);
    st(6, radius);
    st(7, col_r); st(8, col_g); st(9, col_b);
    st(10, opac_out); st(11, valid);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── RENDER: per-pixel front-to-back composite over DEPTH-SORTED projected Gaussians. One thread per pixel. ──────────
//
// This is the `over` operator — the same front-to-back alpha composite as B17's A-buffer, reading the projected
// splats instead of stored fragments. The Gaussians must arrive DEPTH-SORTED (nearest first); B19-a sorts host-side,
// B19-a2 wires the GPU radix sort. Each Gaussian contributes  colour·α_eff·T  where α_eff = opacity·exp(−½·dᵀΣ′⁻¹d)
// (d = pixel − 2D mean, Σ′⁻¹ the conic), and the running transmittance T is multiplied by (1−α_eff) after.
//
// ⛔ NO EARLY-OUT. A real 3DGS rasteriser breaks the per-pixel loop once T falls below a threshold; a CKIR compute
//    loop is branchless, so it runs the full list and relies on α_eff·T → 0 once T is tiny. Correct, just not the
//    perf structure — the tile-bin + T-threshold early-out is the B19-a2/B19-b optimisation.
//
// Buffers:
//   b0 projected (F32, 12 / Gaussian, DEPTH-SORTED): the project kernel's output, reordered nearest-first.
//   b1 params    (F32, 8): [count · imgW · imgH · bgR bgG bgB · alphaMin]
//   b2 out       (F32, 4 / pixel): [R G B T] — T is the working transmittance; the image is slots 0..2.
struct GsplatRenderConfig
{
    int    width      = 512;
    int    height     = 512;
    int    max_splats = 256; // the loop bound (a host constant so the For has a uniform trip count)
    double t_min      = 1.0e-4;
    int    local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat_render_kernel(KGraph& g, const GsplatRenderConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int proj_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int par_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int w    = cu(static_cast<crd::u32>(cfg.width));
    const int px   = g.cast(g.binary(KOp::Mod, tid, w), DType::F32);
    const int py   = g.cast(g.binary(KOp::Div, tid, w), DType::F32);
    const int pxc  = add(g, px, ks(0.5)); // pixel centre
    const int pyc  = add(g, py, ks(0.5));

    const auto pl = [&](int k) {
        const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k)));
        g.stmt_materialize(v);
        return v;
    };
    const int count  = g.cast(pl(0), DType::U32);
    const int bg_r = pl(3);
    const int bg_g = pl(4);
    const int bg_b = pl(5);
    const int amin = pl(6);

    const int p4 = g.binary(KOp::Mul, tid, cu(4U));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), ks(0.0)); // R
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), ks(0.0)); // G
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), ks(0.0)); // B
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(3U)), ks(1.0)); // T = 1

    const int loop = g.stmt_for_begin(count);
    const int gi   = g.kernel_loop_var(loop);
    const int gbse = g.binary(KOp::Mul, gi, cu(12U));
    const auto jl  = [&](int k) {
        const int v = g.buffer_load(proj_b, g.binary(KOp::Add, gbse, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int mnx = jl(0);
    const int mny = jl(1);
    const int cna = jl(3);
    const int cnb = jl(4);
    const int cnc = jl(5);
    const int grad = jl(6);
    const int colr = jl(7);
    const int colg = jl(8);
    const int colb = jl(9);
    const int gopac  = jl(10);
    const int gvalid = jl(11);

    // α_eff = opacity · exp(power), power = −½(a·dx² + c·dy²) − b·dx·dy, clamped ≤ 0
    const int dx = sub(g, mnx, pxc);
    const int dy = sub(g, mny, pyc);
    const int power = g.binary(KOp::Min,
                               sub(g, mul(g, ks(-0.5), add(g, mul(g, cna, sq(g, dx)), mul(g, cnc, sq(g, dy)))),
                                   mul(g, cnb, mul(g, dx, dy))),
                               ks(0.0));
    const int aeff = g.binary(KOp::Min, mul(g, gopac, g.unary(KOp::Exp, power)), ks(0.99));
    // ⛔ RADIUS-BOUND CULL. A projected Gaussian only affects pixels inside its screen footprint (the 3σ box); without
    //    this the infinite Gaussian tail leaks a faint wash into every pixel of the frame, and a supposedly-empty
    //    background is never clean. This is the axis-aligned box the tile binner will use in B19-a2 — here it is a
    //    per-pixel test since there are no tiles yet.
    const int inb  = g.binary(KOp::BitAnd, g.binary(KOp::CmpLt, g.unary(KOp::Abs, dx), grad),
                              g.binary(KOp::CmpLt, g.unary(KOp::Abs, dy), grad));
    // mask: valid Gaussian AND inside its footprint AND α above the cutoff. Otherwise contribute nothing, leave T.
    const int keep = g.binary(KOp::BitAnd, g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, gvalid, ks(0.5)), inb),
                              g.binary(KOp::CmpGe, aeff, amin));
    const int a    = g.select(keep, aeff, ks(0.0));

    const int t_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(t_old);
    const int wgt  = mul(g, a, t_old); // colour weight = α·T
    const int r_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int g_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int b_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(r_old); g.stmt_materialize(g_old); g.stmt_materialize(b_old);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), add(g, r_old, mul(g, colr, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), add(g, g_old, mul(g, colg, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), add(g, b_old, mul(g, colb, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(3U)), mul(g, t_old, sub(g, ks(1.0), a)));
    g.stmt_for_end(loop);

    // composite over the background with the residual transmittance
    const int tf   = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(tf);
    const int rf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int gf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int bf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(rf); g.stmt_materialize(gf); g.stmt_materialize(bf);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), add(g, rf, mul(g, bg_r, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), add(g, gf, mul(g, bg_g, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), add(g, bf, mul(g, bg_b, tf)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    (void)cfg.t_min;
    (void)cfg.max_splats;
    (void)cfg.height;
    return e;
}

// ── B19-a3: THE ON-DEVICE DEPTH SORT. Two small kernels bracket the key-value radix sort (ckir_sort.hpp) so the
//    "sort" half of 3DGS runs on the GPU — no host crutch. depthkey → KV radix sort → gather. ─────────────────────
//
// DEPTHKEY: per splat → a u32 sort key (quantised view-depth, so ascending key = nearest-first) + the splat index as
// the payload the KV sort carries. A culled/padding splat gets key 0xFFFFFFFF so it sorts to the very end.
//   Buffers: b0 projected (F32, 12/splat) · b1 params (F32, 3: [depthMin depthMax count]) · b2 keys (U32, 1) · b3 vals (U32, 1)
struct GsplatDepthKeyConfig
{
    int    local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat_depthkey_kernel(KGraph& g, const GsplatDepthKeyConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int proj_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int par_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int key_b  = g.buffer_decl(DType::U32, 0, 2, true);
    const int val_b  = g.buffer_decl(DType::U32, 0, 3, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int dmin = g.buffer_load(par_b, cu(0U));
    const int dmax = g.buffer_load(par_b, cu(1U));
    g.stmt_materialize(dmin); g.stmt_materialize(dmax);
    const int depth = g.buffer_load(proj_b, g.binary(KOp::Add, g.binary(KOp::Mul, tid, cu(12U)), cu(2U)));
    const int valid = g.buffer_load(proj_b, g.binary(KOp::Add, g.binary(KOp::Mul, tid, cu(12U)), cu(11U)));
    g.stmt_materialize(depth); g.stmt_materialize(valid);
    // quantise depth ∈ [dmin,dmax] → [0, 2^24) (24 bits is ample for depth ordering); ascending ⇒ nearest-first.
    const int t01 = g.binary(KOp::Max, g.binary(KOp::Min, dv(g, sub(g, depth, dmin), g.binary(KOp::Max, sub(g, dmax, dmin), ks(1.0e-9))), ks(1.0)), ks(0.0));
    const int qk  = g.cast(g.unary(KOp::Floor, mul(g, t01, ks(16777215.0))), DType::U32);
    const int key = g.select(g.binary(KOp::CmpGt, valid, ks(0.5)), qk, cu(0xFFFFFFFFU)); // invalid ⇒ sort last
    g.stmt_buffer_store(key_b, tid, key);
    g.stmt_buffer_store(val_b, tid, g.cast(tid, DType::U32));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// GATHER: reorder the projected splats into depth-sorted order using the sorted index payload. sorted[i] = proj[order[i]].
//   Buffers: b0 projected (F32, 12/splat) · b1 order (U32, 1/splat, the KV-sorted index) · b2 sorted (F32, 12/splat)
[[nodiscard]] inline KEntry build_gsplat_gather_kernel(KGraph& g, int local_size = 64)
{
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const int   proj_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int   ord_b  = g.buffer_decl(DType::U32, 0, 1, false);
    const int   out_b  = g.buffer_decl(DType::F32, 0, 2, true);
    const int   tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));
    const int   mark = g.kernel_stmt_mark();
    const int   src  = g.binary(KOp::Mul, g.cast(g.buffer_load(ord_b, tid), DType::U32), cu(12U));
    g.stmt_materialize(src);
    const int dst = g.binary(KOp::Mul, tid, cu(12U));
    for (int k = 0; k < 12; ++k)
    {
        g.stmt_buffer_store(out_b, g.binary(KOp::Add, dst, cu(static_cast<crd::u32>(k))),
                            g.buffer_load(proj_b, g.binary(KOp::Add, src, cu(static_cast<crd::u32>(k)))));
    }
    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── TILED RENDER: the perf structure. Each pixel composites ONLY its screen tile's splats, not the whole scene. ─────
//
// B19-a's render loops every Gaussian for every pixel — O(pixels · Gaussians), correct but the brute-force cost 3DGS
// exists to avoid. Real 3DGS bins each Gaussian into the 16×16 screen tiles its footprint overlaps, so a pixel only
// touches its tile's list. This kernel is that render: per pixel → its tile → composite the tile's bucket (still
// front-to-back, since the bucket is filled in global depth order).
//
// ⛔ THE LOOP BOUND MUST BE UNIFORM. A CKIR `For` needs the same trip count for every thread in a dispatch, but a
//    tile's list length varies per tile. So the bucket has a FIXED CAPACITY `cap` and the loop runs `cap` times,
//    masked by (i < count[tile]) — uniform bound, correct result. Overflow past `cap` is dropped (the binner clamps);
//    the elegant workgroup-per-tile variable-length loop is a later refinement (needs the tile = workgroup model).
//
// The bucket is built by the caller: global depth sort → for each splat, append its 12 projected floats to every tile
// its bbox covers (up to `cap`). B19-a2 fills it host-side (the same honest split as B19-a's host depth sort); the
// GPU-side count+scan+scatter + a payload-carrying radix sort is B19-a3 (it needs a 64-bit/payload sort extension).
//
// Buffers:
//   b0 buckets (F32, nTiles·cap·12): per tile, up to `cap` projected splats in depth order.
//   b1 counts  (F32, nTiles): how many real splats each tile's bucket holds.
//   b2 params  (F32, 8): [imgW · imgH · tiles_x · bgR bgG bgB · alphaMin]
//   b3 out     (F32, 4 / pixel): [R G B T]
struct GsplatTiledConfig
{
    int    width      = 512;
    int    height     = 512;
    int    tile_px    = 16;
    int    cap        = 256; // fixed bucket capacity per tile (the uniform loop bound)
    int    local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat_tiled_render_kernel(KGraph& g, const GsplatTiledConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int buck_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int cnt_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int par_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 3, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark  = g.kernel_stmt_mark();
    const int wi    = cu(static_cast<crd::u32>(cfg.width));
    const int tpx   = cu(static_cast<crd::u32>(cfg.tile_px));
    const int tiles_x = cu(static_cast<crd::u32>((cfg.width + cfg.tile_px - 1) / cfg.tile_px));
    const int pxu   = g.binary(KOp::Mod, tid, wi);
    const int pyu   = g.binary(KOp::Div, tid, wi);
    const int pxc   = add(g, g.cast(pxu, DType::F32), ks(0.5));
    const int pyc   = add(g, g.cast(pyu, DType::F32), ks(0.5));
    const int tile  = g.binary(KOp::Add, g.binary(KOp::Div, pxu, tpx), g.binary(KOp::Mul, g.binary(KOp::Div, pyu, tpx), tiles_x));
    g.stmt_materialize(tile);

    const auto pl = [&](int k) {
        const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k)));
        g.stmt_materialize(v);
        return v;
    };
    const int bg_r = pl(3);
    const int bg_g = pl(4);
    const int bg_b = pl(5);
    const int amin = pl(6);
    const int count = g.cast(g.buffer_load(cnt_b, tile), DType::U32);
    g.stmt_materialize(count);

    const int p4 = g.binary(KOp::Mul, tid, cu(4U));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), ks(0.0));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), ks(0.0));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), ks(0.0));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(3U)), ks(1.0));

    const int base = g.binary(KOp::Mul, tile, cu(static_cast<crd::u32>(cfg.cap)));
    const int loop = g.stmt_for_begin(cu(static_cast<crd::u32>(cfg.cap)));
    const int i    = g.kernel_loop_var(loop);
    const int inrange = g.binary(KOp::CmpLt, i, count); // mask iterations past this tile's real count
    const int gbse = g.binary(KOp::Mul, g.binary(KOp::Add, base, i), cu(12U));
    const auto jl  = [&](int k) {
        const int v = g.buffer_load(buck_b, g.binary(KOp::Add, gbse, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int mnx = jl(0);
    const int mny = jl(1);
    const int cna = jl(3);
    const int cnb = jl(4);
    const int cnc = jl(5);
    const int grad = jl(6);
    const int colr = jl(7);
    const int colg = jl(8);
    const int colb = jl(9);
    const int gopac  = jl(10);
    const int gvalid = jl(11);

    const int dx = sub(g, mnx, pxc);
    const int dy = sub(g, mny, pyc);
    const int power = g.binary(KOp::Min,
                               sub(g, mul(g, ks(-0.5), add(g, mul(g, cna, sq(g, dx)), mul(g, cnc, sq(g, dy)))),
                                   mul(g, cnb, mul(g, dx, dy))),
                               ks(0.0));
    const int aeff = g.binary(KOp::Min, mul(g, gopac, g.unary(KOp::Exp, power)), ks(0.99));
    const int inb  = g.binary(KOp::BitAnd, g.binary(KOp::CmpLt, g.unary(KOp::Abs, dx), grad),
                              g.binary(KOp::CmpLt, g.unary(KOp::Abs, dy), grad));
    const int keep = g.binary(KOp::BitAnd, g.binary(KOp::BitAnd, inrange, g.binary(KOp::CmpGt, gvalid, ks(0.5))),
                              g.binary(KOp::BitAnd, inb, g.binary(KOp::CmpGe, aeff, amin)));
    const int a    = g.select(keep, aeff, ks(0.0));

    const int t_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(t_old);
    const int wgt   = mul(g, a, t_old);
    const int r_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int g_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int b_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(r_old); g.stmt_materialize(g_old); g.stmt_materialize(b_old);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), add(g, r_old, mul(g, colr, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), add(g, g_old, mul(g, colg, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), add(g, b_old, mul(g, colb, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(3U)), mul(g, t_old, sub(g, ks(1.0), a)));
    g.stmt_for_end(loop);

    const int tf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(tf);
    const int rf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int gf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int bf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(rf); g.stmt_materialize(gf); g.stmt_materialize(bf);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), add(g, rf, mul(g, bg_r, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), add(g, gf, mul(g, bg_g, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), add(g, bf, mul(g, bg_b, tf)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    (void)cfg.height;
    return e;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// B19-a4 — the FULL GPU TILE BINNING (the last host crutch in the tiled render). 3DGS binning is the standard
// duplicate → scan → scatter → sort → ranges pipeline; the render then becomes ONE WORKGROUP PER TILE with a
// variable (workgroup-uniform) range length — the real Kerbl-2023 block rasteriser topology, no fixed bucket cap.
//
//   1. TILECOUNT  — per depth-sorted splat, how many screen tiles its 3σ bbox covers (0 if culled/off-screen).
//   2. SCAN       — exclusive prefix-sum of the tilecounts (reuse ckir_scan) → per-splat instance base offset; the
//                   grand total T = number of (tile,splat) instances.
//   3. SCATTER    — grid = N·max_cover threads (splat,slot); slot<tilecount ⇒ write key=tileID, val=splat-index at
//                   off[splat]+slot. GRID-DRIVEN (no CKIR For) ⇒ the per-splat variable fan-out needs no uniform bound.
//   4. SORT       — the KV radix sort (ckir_sort, carry_val) BY tileID. LSD radix is STABLE, and the splats are
//                   already globally depth-sorted (B19-a3), so a stable sort by tile alone yields tile-major AND
//                   depth-order-within-tile — no depth bits in the key, no depth-precision loss.
//   5. RANGES     — per instance position, boundary-detect the tile changes → per-tile [start,end) into the sorted list.
//   6. BLOCK RENDER — workgroup = tile, loop the tile's [start,end) (variable, workgroup-uniform), composite.
//
// The screen radius (proj slot 6) is ⌈3√λ_max⌉; the tile geometry is compile-time (cfg.width/tile_px), matching the
// projection/render kernels. `max_cover` caps tiles-covered-per-splat (the scatter fan-out bound); the gates assert no
// splat exceeds it, so within a bounded scene the bin is EXACT (a full-screen splat past the cap drops extra tiles —
// documented, mirrors the render's honest fixed-cap discipline). Buffers throughout: projected splats are the
// DEPTH-SORTED buffer from `build_gsplat_gather_kernel`.
struct GsplatBinConfig
{
    int width      = 512;
    int height     = 512;
    int tile_px    = 16;
    int max_cover  = 64;  // max tiles one splat may fan out to (the scatter grid stride); gates assert it's not exceeded
    int local_size = 64;
};

// TILECOUNT: per depth-sorted splat → number of covered screen tiles (half-open clamped rect, Kerbl getRect).
//   b0 sorted (F32, 12/splat) · b1 tilecount (F32, 1/splat, write)
[[nodiscard]] inline KEntry build_gsplat_tilecount_kernel(KGraph& g, const GsplatBinConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  mn  = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto  mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int sorted_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int tc_b     = g.buffer_decl(DType::F32, 0, 1, true);
    const int tid      = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark    = g.kernel_stmt_mark();
    const int tiles_x = (cfg.width + cfg.tile_px - 1) / cfg.tile_px;
    const int tiles_y = (cfg.height + cfg.tile_px - 1) / cfg.tile_px;
    const int base    = g.binary(KOp::Mul, tid, cu(12U));
    const auto ld  = [&](int k) {
        const int v = g.buffer_load(sorted_b, g.binary(KOp::Add, base, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int mean_x = ld(0);
    const int mean_y = ld(1);
    const int radius = ld(6);
    const int valid  = ld(11);

    // half-open clamped tile rect [rx0,rx1) × [ry0,ry1); count 0 if it collapses (fully off-screen).
    const int itpx = ks(1.0 / static_cast<double>(cfg.tile_px));
    const auto clampf = [&](int v, double hi) { return mn(mx(v, ks(0.0)), ks(hi)); };
    const int rx0 = clampf(g.unary(KOp::Floor, mul(g, sub(g, mean_x, radius), itpx)), static_cast<double>(tiles_x));
    const int rx1 = clampf(add(g, g.unary(KOp::Floor, mul(g, add(g, mean_x, radius), itpx)), ks(1.0)), static_cast<double>(tiles_x));
    const int ry0 = clampf(g.unary(KOp::Floor, mul(g, sub(g, mean_y, radius), itpx)), static_cast<double>(tiles_y));
    const int ry1 = clampf(add(g, g.unary(KOp::Floor, mul(g, add(g, mean_y, radius), itpx)), ks(1.0)), static_cast<double>(tiles_y));
    const int cx  = mx(sub(g, rx1, rx0), ks(0.0));
    const int cy  = mx(sub(g, ry1, ry0), ks(0.0));
    const int tc  = g.select(g.binary(KOp::CmpGt, valid, ks(0.5)), mul(g, cx, cy), ks(0.0));
    g.stmt_buffer_store(tc_b, tid, tc);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// SCATTER instances: emit one (key=tileID, val=splatIndex) per (splat, covered-tile). GRID = N·max_cover threads —
// thread → (splat = tid/max_cover, slot = tid%max_cover); a thread with slot < tilecount[splat] decodes the slot-th
// covered tile (row-major within the splat's rect, the SAME rect tilecount computed) and writes at off[splat]+slot.
// No CKIR `For`: the per-splat variable fan-out is expressed by the grid, so there is no non-uniform loop bound. The
// guarded write uses `If` (slot < tilecount). Positions [T, padded) are left untouched ⇒ the caller pre-fills the key
// buffer with 0xFFFFFFFF so the radix sort pushes the padding to the end.
//   b0 sorted (F32,12) · b1 tilecount (F32,1) · b2 off (F32,1, exclusive scan of tilecount) · b3 keys (U32,1,w) · b4 vals (U32,1,w)
[[nodiscard]] inline KEntry build_gsplat_scatter_instances_kernel(KGraph& g, const GsplatBinConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  mn  = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto  mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int sorted_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int tc_b     = g.buffer_decl(DType::F32, 0, 1, false);
    const int off_b    = g.buffer_decl(DType::F32, 0, 2, false);
    const int key_b    = g.buffer_decl(DType::U32, 0, 3, true);
    const int pay_b    = g.buffer_decl(DType::U32, 0, 4, true); // the splat-index payload the tile-sort carries
    const int tid      = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark    = g.kernel_stmt_mark();
    const int tiles_x = (cfg.width + cfg.tile_px - 1) / cfg.tile_px;
    const int tiles_y = (cfg.height + cfg.tile_px - 1) / cfg.tile_px;
    const int splat   = g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(cfg.max_cover)));
    const int slot    = g.binary(KOp::Mod, tid, cu(static_cast<crd::u32>(cfg.max_cover)));
    g.stmt_materialize(splat);
    g.stmt_materialize(slot);

    const int tcf = g.buffer_load(tc_b, splat);
    g.stmt_materialize(tcf);
    const int tcu  = g.cast(tcf, DType::U32);
    const int cond = g.binary(KOp::CmpLt, slot, tcu); // this thread's slot is a real covered tile of its splat

    const int cif = g.stmt_if_begin(cond);
    {
        const int sbase = g.binary(KOp::Mul, splat, cu(12U));
        const auto ld   = [&](int k) {
            const int v = g.buffer_load(sorted_b, g.binary(KOp::Add, sbase, cu(static_cast<crd::u32>(k))));
            g.stmt_materialize(v);
            return v;
        };
        const int mean_x = ld(0);
        const int mean_y = ld(1);
        const int radius = ld(6);
        const int itpx   = ks(1.0 / static_cast<double>(cfg.tile_px));
        const auto clampf = [&](int v, double hi) { return mn(mx(v, ks(0.0)), ks(hi)); };
        // the SAME half-open clamped rect the tilecount kernel used (⇒ slot decode is consistent with the count).
        const int rx0f = clampf(g.unary(KOp::Floor, mul(g, sub(g, mean_x, radius), itpx)), static_cast<double>(tiles_x));
        const int ry0f = clampf(g.unary(KOp::Floor, mul(g, sub(g, mean_y, radius), itpx)), static_cast<double>(tiles_y));
        const int rx1f = clampf(add(g, g.unary(KOp::Floor, mul(g, add(g, mean_x, radius), itpx)), ks(1.0)), static_cast<double>(tiles_x));
        const int cxf  = mx(sub(g, rx1f, rx0f), ks(0.0));
        const int rx0u = g.cast(rx0f, DType::U32);
        const int ry0u = g.cast(ry0f, DType::U32);
        const int cxu  = mx(g.cast(cxf, DType::U32), cu(1U)); // ≥1 (guarded by cond ⇒ tc>0 ⇒ cx>0; the max keeps div safe)
        const int ly   = g.binary(KOp::Div, slot, cxu);
        const int lx   = g.binary(KOp::Sub, slot, g.binary(KOp::Mul, ly, cxu));
        const int tx   = g.binary(KOp::Add, rx0u, lx);
        const int ty   = g.binary(KOp::Add, ry0u, ly);
        const int tile = g.binary(KOp::Add, tx, g.binary(KOp::Mul, ty, cu(static_cast<crd::u32>(tiles_x))));

        const int offu = g.cast(g.buffer_load(off_b, splat), DType::U32);
        const int pos  = g.binary(KOp::Add, offu, slot);
        g.stmt_buffer_store(key_b, pos, tile);
        g.stmt_buffer_store(pay_b, pos, splat);
    }
    g.stmt_if_end(cif);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// TILE RANGES: given the tile-sorted instance keys, find each tile's [start,end) span in the sorted list. A position is
// a tile's START if its key differs from the previous position's (or it is position 0); an END if it differs from the
// next (or it is the last real instance, T-1). Runs over the padded sort buffer, guarded to the real [0,T) range; the
// `ranges` buffer is pre-zeroed by the caller so untouched tiles read [0,0) (⇒ the block render loops 0 instances).
//   b0 keys (U32, sorted, padded) · b1 params (F32,1: [T]) · b2 ranges (U32, 2·nTiles, w) as [start0 end0 start1 end1 …]
[[nodiscard]] inline KEntry build_gsplat_tile_ranges_kernel(KGraph& g, int local_size = 64)
{
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };

    const int key_b = g.buffer_decl(DType::U32, 0, 0, false);
    const int par_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int rng_b = g.buffer_decl(DType::U32, 0, 2, true);
    const int p     = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(local_size))),
                           g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int tcnt = g.cast(g.buffer_load(par_b, cu(0U)), DType::U32); // T = number of real instances
    g.stmt_materialize(tcnt);
    const int inrange = g.binary(KOp::CmpLt, p, tcnt);

    const int cif = g.stmt_if_begin(inrange);
    {
        const int t = g.buffer_load(key_b, p);
        g.stmt_materialize(t);
        const int is0  = g.binary(KOp::CmpEq, p, cu(0U));
        const int pm1  = g.select(is0, cu(0U), g.binary(KOp::Sub, p, cu(1U))); // clamp so p==0 never reads key[-1]
        const int kprev = g.buffer_load(key_b, pm1);
        g.stmt_materialize(kprev);
        const int knext = g.buffer_load(key_b, g.binary(KOp::Add, p, cu(1U))); // padded > T ⇒ key[p+1] is always in bounds
        g.stmt_materialize(knext);
        const int is_last = g.binary(KOp::CmpEq, p, g.binary(KOp::Sub, tcnt, cu(1U)));
        // bool combos are consumed inline by the `If` conditions — NEVER materialize a Bool value (a temp is typed
        // int/float and cannot hold a GLSL bool), the same discipline the tiled render's keep-mask follows.
        const int is_start = g.binary(KOp::BitOr, is0, g.binary(KOp::CmpNe, kprev, t));
        const int is_end   = g.binary(KOp::BitOr, is_last, g.binary(KOp::CmpNe, knext, t));
        const int ridx = g.binary(KOp::Mul, t, cu(2U));
        g.stmt_materialize(ridx); // used in BOTH inner If bodies ⇒ hoist its decl to the enclosing scope (If-shared-temp scar)

        const int sif = g.stmt_if_begin(is_start);
        {
            g.stmt_buffer_store(rng_b, ridx, p);
        }
        g.stmt_if_end(sif);
        const int eif = g.stmt_if_begin(is_end);
        {
            g.stmt_buffer_store(rng_b, g.binary(KOp::Add, ridx, cu(1U)), g.binary(KOp::Add, p, cu(1U)));
        }
        g.stmt_if_end(eif);
    }
    g.stmt_if_end(cif);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// BLOCK RENDER: the real Kerbl-2023 tile rasteriser topology — ONE WORKGROUP PER TILE, one thread per pixel, looping the
// tile's [start,end) instance span (a VARIABLE bound that is workgroup-uniform, since every thread in the workgroup shares
// the tile — exactly the constraint the CKIR `For` bound requires). No fixed bucket cap: a tile composites all its
// instances. Each instance → `order[inst]` → the depth-sorted splat; front-to-back `over`. The output is tile-aligned
// (padded_w = tiles_x·tile_px), so every thread maps to a real output pixel.
//   b0 sorted (F32,12/splat, depth-sorted) · b1 order (U32, tile-sorted instance→splat) · b2 ranges (U32,2·nTiles)
//   b3 params (F32,4: [bgR bgG bgB alphaMin]) · b4 out (F32,4·padded_w·padded_h RGBA)
struct GsplatBlockConfig
{
    int    width           = 512;
    int    height          = 512;
    int    tile_px         = 16;
    bool   early_terminate = false; // smem render only: stop a tile once every pixel's transmittance saturates (< t_stop)
    double t_stop          = 1.0e-4;
};

[[nodiscard]] inline KEntry build_gsplat_block_render_kernel(KGraph& g, const GsplatBlockConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int sorted_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int order_b  = g.buffer_decl(DType::U32, 0, 1, false);
    const int rng_b    = g.buffer_decl(DType::U32, 0, 2, false);
    const int par_b    = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_b    = g.buffer_decl(DType::F32, 0, 4, true);

    const int tiles_x  = (cfg.width + cfg.tile_px - 1) / cfg.tile_px;
    const int padded_w = tiles_x * cfg.tile_px;
    const int tpx      = cfg.tile_px;
    const int tile     = g.builtin(KBuiltin::WorkgroupIndex); // one workgroup == one tile
    const int lid      = g.builtin(KBuiltin::LocalInvocationIndex);

    const int mark = g.kernel_stmt_mark();
    // pixel = tile origin + intra-tile (lx,ly); output is tile-aligned so px<padded_w, py<padded_h always.
    const int tx  = g.binary(KOp::Mod, tile, cu(static_cast<crd::u32>(tiles_x)));
    const int ty  = g.binary(KOp::Div, tile, cu(static_cast<crd::u32>(tiles_x)));
    const int lx  = g.binary(KOp::Mod, lid, cu(static_cast<crd::u32>(tpx)));
    const int ly  = g.binary(KOp::Div, lid, cu(static_cast<crd::u32>(tpx)));
    const int px  = g.binary(KOp::Add, g.binary(KOp::Mul, tx, cu(static_cast<crd::u32>(tpx))), lx);
    const int py  = g.binary(KOp::Add, g.binary(KOp::Mul, ty, cu(static_cast<crd::u32>(tpx))), ly);
    g.stmt_materialize(tile);
    g.stmt_materialize(px);
    g.stmt_materialize(py);
    const int pxc = add(g, g.cast(px, DType::F32), ks(0.5));
    const int pyc = add(g, g.cast(py, DType::F32), ks(0.5));
    const int pix = g.binary(KOp::Add, px, g.binary(KOp::Mul, py, cu(static_cast<crd::u32>(padded_w))));
    const int p4  = g.binary(KOp::Mul, pix, cu(4U));
    g.stmt_materialize(p4);

    const auto pl = [&](int k) {
        const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k)));
        g.stmt_materialize(v);
        return v;
    };
    const int bg_r = pl(0);
    const int bg_g = pl(1);
    const int bg_b = pl(2);
    const int amin = pl(3);

    const int r2   = g.binary(KOp::Mul, tile, cu(2U));
    const int rs   = g.buffer_load(rng_b, r2);
    const int re   = g.buffer_load(rng_b, g.binary(KOp::Add, r2, cu(1U)));
    g.stmt_materialize(rs);
    g.stmt_materialize(re);
    // this tile's instance count — VARIABLE but workgroup-uniform. Guard against an inverted range: an unsigned Sub
    // would wrap to ~2^32 and hang the loop, so clamp to 0 when re ≤ rs (a well-formed range always has re ≥ rs).
    const int span = g.select(g.binary(KOp::CmpGt, re, rs), g.binary(KOp::Sub, re, rs), cu(0U));
    g.stmt_materialize(span);

    // init accumulator (RGB, transmittance) — one thread owns this pixel, so the loop RMW is race-free.
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), ks(0.0));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), ks(0.0));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), ks(0.0));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(3U)), ks(1.0));

    const int loop = g.stmt_for_begin(span);
    const int i    = g.kernel_loop_var(loop);
    const int inst = g.binary(KOp::Add, rs, i);
    const int splat = g.buffer_load(order_b, inst);
    g.stmt_materialize(splat);
    const int sbase = g.binary(KOp::Mul, splat, cu(12U));
    const auto jl   = [&](int k) {
        const int v = g.buffer_load(sorted_b, g.binary(KOp::Add, sbase, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int mnx    = jl(0);
    const int mny    = jl(1);
    const int cna    = jl(3);
    const int cnb    = jl(4);
    const int cnc    = jl(5);
    const int grad   = jl(6);
    const int colr   = jl(7);
    const int colg   = jl(8);
    const int colb   = jl(9);
    const int gopac  = jl(10);
    const int gvalid = jl(11);

    const int dx = sub(g, mnx, pxc);
    const int dy = sub(g, mny, pyc);
    const int power = g.binary(KOp::Min,
                               sub(g, mul(g, ks(-0.5), add(g, mul(g, cna, sq(g, dx)), mul(g, cnc, sq(g, dy)))),
                                   mul(g, cnb, mul(g, dx, dy))),
                               ks(0.0));
    const int aeff = g.binary(KOp::Min, mul(g, gopac, g.unary(KOp::Exp, power)), ks(0.99));
    const int inb  = g.binary(KOp::BitAnd, g.binary(KOp::CmpLt, g.unary(KOp::Abs, dx), grad),
                              g.binary(KOp::CmpLt, g.unary(KOp::Abs, dy), grad));
    const int keep = g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, gvalid, ks(0.5)),
                              g.binary(KOp::BitAnd, inb, g.binary(KOp::CmpGe, aeff, amin)));
    const int a    = g.select(keep, aeff, ks(0.0));

    const int t_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(t_old);
    const int wgt   = mul(g, a, t_old);
    const int r_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int g_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int b_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(r_old); g.stmt_materialize(g_old); g.stmt_materialize(b_old);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), add(g, r_old, mul(g, colr, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), add(g, g_old, mul(g, colg, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), add(g, b_old, mul(g, colb, wgt)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(3U)), mul(g, t_old, sub(g, ks(1.0), a)));
    g.stmt_for_end(loop);

    const int tf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(tf);
    const int rf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int gf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int bf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(rf); g.stmt_materialize(gf); g.stmt_materialize(bf);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(0U)), add(g, rf, mul(g, bg_r, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), add(g, gf, mul(g, bg_g, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), add(g, bf, mul(g, bg_b, tf)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.tile_px * cfg.tile_px);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    (void)cfg.height;
    return e;
}

// ⭐ PERFORMANT BLOCK RENDER (the Kerbl-2023 / Inria rasteriser architecture). The direct block render above reads every
// splat's 12 floats from GLOBAL memory once per pixel per splat — at scale (millions of splats, high overdraw) that
// bandwidth dominates. Here the workgroup COLLABORATIVELY LOADS a batch of BS = tile_px² splats into SHARED memory
// (each thread loads one, coalesced), barriers, then every pixel composites the whole batch from shared. The per-pixel
// RGBA accumulator ALSO lives in shared (each thread owns its slot, re-read across the loop since a CKIR `For` carries
// no register state), so per-splat GLOBAL traffic is ~zero: splats and output touch DRAM once each. Bit-exact vs the
// direct block render (same instance order). Buffers identical to build_gsplat_block_render_kernel.
[[nodiscard]] inline KEntry build_gsplat_block_render_smem_kernel(KGraph& g, const GsplatBlockConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int sorted_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int order_b  = g.buffer_decl(DType::U32, 0, 1, false);
    const int rng_b    = g.buffer_decl(DType::U32, 0, 2, false);
    const int par_b    = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_b    = g.buffer_decl(DType::F32, 0, 4, true);

    const int tiles_x  = (cfg.width + cfg.tile_px - 1) / cfg.tile_px;
    const int padded_w = tiles_x * cfg.tile_px;
    const int tpx      = cfg.tile_px;
    const int bs       = cfg.tile_px * cfg.tile_px; // batch = one splat per thread
    const int tile     = g.builtin(KBuiltin::WorkgroupIndex);
    const int lid      = g.builtin(KBuiltin::LocalInvocationIndex);
    const int sh_sp    = g.shared_decl(DType::F32, bs * 11); // batch of splats (11 fields each)
    const int sh_ac    = g.shared_decl(DType::F32, bs * 4);  // per-thread RGBA accumulator
    const int sh_act   = cfg.early_terminate ? g.shared_decl(DType::U32, 1) : 0; // count of still-active pixels this round

    const int mark = g.kernel_stmt_mark();
    const int tx = g.binary(KOp::Mod, tile, cu(static_cast<crd::u32>(tiles_x)));
    const int ty = g.binary(KOp::Div, tile, cu(static_cast<crd::u32>(tiles_x)));
    const int lx = g.binary(KOp::Mod, lid, cu(static_cast<crd::u32>(tpx)));
    const int ly = g.binary(KOp::Div, lid, cu(static_cast<crd::u32>(tpx)));
    const int px = g.binary(KOp::Add, g.binary(KOp::Mul, tx, cu(static_cast<crd::u32>(tpx))), lx);
    const int py = g.binary(KOp::Add, g.binary(KOp::Mul, ty, cu(static_cast<crd::u32>(tpx))), ly);
    g.stmt_materialize(px); g.stmt_materialize(py); g.stmt_materialize(lid);
    const int pxc = add(g, g.cast(px, DType::F32), ks(0.5));
    const int pyc = add(g, g.cast(py, DType::F32), ks(0.5));
    const int pix = g.binary(KOp::Add, px, g.binary(KOp::Mul, py, cu(static_cast<crd::u32>(padded_w))));
    const int p4  = g.binary(KOp::Mul, pix, cu(4U));
    g.stmt_materialize(p4);
    const int a4  = g.binary(KOp::Mul, lid, cu(4U)); // this thread's accumulator base in shared
    g.stmt_materialize(a4);

    const auto pl = [&](int k) { const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int bg_r = pl(0); const int bg_g = pl(1); const int bg_b = pl(2); const int amin = pl(3);

    const int r2   = g.binary(KOp::Mul, tile, cu(2U));
    const int rs   = g.buffer_load(rng_b, r2);
    const int re   = g.buffer_load(rng_b, g.binary(KOp::Add, r2, cu(1U)));
    g.stmt_materialize(rs); g.stmt_materialize(re);
    const int span = g.select(g.binary(KOp::CmpGt, re, rs), g.binary(KOp::Sub, re, rs), cu(0U));
    g.stmt_materialize(span);
    // n_chunks = ceil(span / BS) — variable but workgroup-uniform
    const int nchunks = g.binary(KOp::Div, g.binary(KOp::Add, span, cu(static_cast<crd::u32>(bs - 1))), cu(static_cast<crd::u32>(bs)));
    g.stmt_materialize(nchunks);

    // init the shared accumulator
    g.stmt_shared_store(sh_ac, a4, ks(0.0));
    g.stmt_shared_store(sh_ac, g.binary(KOp::Add, a4, cu(1U)), ks(0.0));
    g.stmt_shared_store(sh_ac, g.binary(KOp::Add, a4, cu(2U)), ks(0.0));
    g.stmt_shared_store(sh_ac, g.binary(KOp::Add, a4, cu(3U)), ks(1.0));

    const int cloop = g.stmt_for_begin(nchunks);
    const int ch    = g.kernel_loop_var(cloop);
    const int cbase = g.binary(KOp::Add, rs, g.binary(KOp::Mul, ch, cu(static_cast<crd::u32>(bs))));
    // ── collaborative load: thread lid → instance cbase+lid into sh_sp[lid·11 + …] ──
    const int inst_l  = g.binary(KOp::Add, cbase, lid);
    const int inr_l   = g.binary(KOp::CmpLt, inst_l, re);
    const int inst_lc = g.select(inr_l, inst_l, g.binary(KOp::Sub, re, cu(1U)));
    const int splat   = g.buffer_load(order_b, inst_lc);
    g.stmt_materialize(splat);
    const int sbase   = g.binary(KOp::Mul, splat, cu(12U));
    const int sl11    = g.binary(KOp::Mul, lid, cu(11U));
    const auto sload  = [&](int k) { const int v = g.buffer_load(sorted_b, g.binary(KOp::Add, sbase, cu(static_cast<crd::u32>(k)))); g.stmt_materialize(v); return v; };
    // fields: 0 mnx,1 mny,2 conA,3 conB,4 conC,5 rad,6 colR,7 colG,8 colB,9 opac,10 valid  (from sorted slots 0,1,3,4,5,6,7,8,9,10,11)
    g.stmt_shared_store(sh_sp, sl11, sload(0));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(1U)), sload(1));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(2U)), sload(3));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(3U)), sload(4));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(4U)), sload(5));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(5U)), sload(6));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(6U)), sload(7));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(7U)), sload(8));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(8U)), sload(9));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(9U)), sload(10));
    g.stmt_shared_store(sh_sp, g.binary(KOp::Add, sl11, cu(10U)), g.select(inr_l, sload(11), ks(0.0))); // valid=0 for padding
    g.stmt_barrier();

    // ── composite: every pixel over the shared batch. Bound = the chunk's ACTUAL splat count (min(BS, re−cbase)), not a
    //    fixed BS — so a low-overdraw tile doesn't waste the whole 256-wide inner loop on padding. Workgroup-uniform. ──
    const int chunk_n = g.binary(KOp::Min, cu(static_cast<crd::u32>(bs)), g.binary(KOp::Sub, re, cbase));
    g.stmt_materialize(chunk_n);
    const int jloop = g.stmt_for_begin(chunk_n);
    const int j     = g.kernel_loop_var(jloop);
    const int inst_j = g.binary(KOp::Add, cbase, j);
    const int inr_j  = g.binary(KOp::CmpLt, inst_j, re);
    const int sj11   = g.binary(KOp::Mul, j, cu(11U));
    const auto shl   = [&](int k) { const int v = g.shared_load(sh_sp, g.binary(KOp::Add, sj11, cu(static_cast<crd::u32>(k)))); g.stmt_materialize(v); return v; };
    const int mnx = shl(0); const int mny = shl(1);
    const int cna = shl(2); const int cnb = shl(3); const int cnc = shl(4); const int grad = shl(5);
    const int colr = shl(6); const int colg = shl(7); const int colb = shl(8); const int gopac = shl(9); const int gvalid = shl(10);
    const int dx = sub(g, mnx, pxc);
    const int dy = sub(g, mny, pyc);
    const int power = g.binary(KOp::Min, sub(g, mul(g, ks(-0.5), add(g, mul(g, cna, sq(g, dx)), mul(g, cnc, sq(g, dy)))), mul(g, cnb, mul(g, dx, dy))), ks(0.0));
    const int aeff = g.binary(KOp::Min, mul(g, gopac, g.unary(KOp::Exp, power)), ks(0.99));
    const int inb  = g.binary(KOp::BitAnd, g.binary(KOp::CmpLt, g.unary(KOp::Abs, dx), grad), g.binary(KOp::CmpLt, g.unary(KOp::Abs, dy), grad));
    const int keep = g.binary(KOp::BitAnd, g.binary(KOp::BitAnd, inr_j, g.binary(KOp::CmpGt, gvalid, ks(0.5))),
                              g.binary(KOp::BitAnd, inb, g.binary(KOp::CmpGe, aeff, amin)));
    const int a = g.select(keep, aeff, ks(0.0));
    const int t_old = g.shared_load(sh_ac, g.binary(KOp::Add, a4, cu(3U)));
    g.stmt_materialize(t_old);
    const int wgt = mul(g, a, t_old);
    const int r_old = g.shared_load(sh_ac, a4);
    const int g_old = g.shared_load(sh_ac, g.binary(KOp::Add, a4, cu(1U)));
    const int b_old = g.shared_load(sh_ac, g.binary(KOp::Add, a4, cu(2U)));
    g.stmt_materialize(r_old); g.stmt_materialize(g_old); g.stmt_materialize(b_old);
    g.stmt_shared_store(sh_ac, a4, add(g, r_old, mul(g, colr, wgt)));
    g.stmt_shared_store(sh_ac, g.binary(KOp::Add, a4, cu(1U)), add(g, g_old, mul(g, colg, wgt)));
    g.stmt_shared_store(sh_ac, g.binary(KOp::Add, a4, cu(2U)), add(g, b_old, mul(g, colb, wgt)));
    g.stmt_shared_store(sh_ac, g.binary(KOp::Add, a4, cu(3U)), mul(g, t_old, sub(g, ks(1.0), a)));
    g.stmt_for_end(jloop);
    g.stmt_barrier(); // done reading sh_sp before the next chunk overwrites it
    // ── EARLY TERMINATION: break the chunk loop once EVERY pixel in the tile has saturated (T < t_stop). This is the
    //    reference rasteriser's big win at high overdraw; it drops the negligible saturated tail, so it is not bit-exact
    //    vs the no-early-out render (≤ t_stop·colour), hence gated as a config option. ──
    if (cfg.early_terminate)
    {
        g.stmt_shared_store(sh_act, cu(0U), cu(0U)); // reset (all threads write 0 — same value, race-safe)
        g.stmt_barrier();
        const int myt = g.shared_load(sh_ac, g.binary(KOp::Add, a4, cu(3U)));
        g.stmt_materialize(myt);
        const int cif = g.stmt_if_begin(g.binary(KOp::CmpGe, myt, ks(cfg.t_stop))); // still active ⇒ count it
        {
            g.stmt_shared_atomic_add(sh_act, cu(0U), cu(1U));
        }
        g.stmt_if_end(cif);
        g.stmt_barrier();
        const int nact = g.shared_load(sh_act, cu(0U));
        g.stmt_materialize(nact);
        g.stmt_for_break_if(g.binary(KOp::CmpEq, nact, cu(0U))); // all pixels saturated ⇒ stop this tile
        g.stmt_barrier(); // before the next chunk's load overwrites sh_sp
    }
    g.stmt_for_end(cloop);

    // ── resolve: composite the background under the accumulated colour, write the pixel once ──
    const int tf = g.shared_load(sh_ac, g.binary(KOp::Add, a4, cu(3U)));
    const int rf = g.shared_load(sh_ac, a4);
    const int gf = g.shared_load(sh_ac, g.binary(KOp::Add, a4, cu(1U)));
    const int bf = g.shared_load(sh_ac, g.binary(KOp::Add, a4, cu(2U)));
    g.stmt_materialize(tf); g.stmt_materialize(rf); g.stmt_materialize(gf); g.stmt_materialize(bf);
    g.stmt_buffer_store(out_b, p4, add(g, rf, mul(g, bg_r, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(1U)), add(g, gf, mul(g, bg_g, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(2U)), add(g, bf, mul(g, bg_b, tf)));
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(3U)), tf);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(bs);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    (void)cfg.height;
    return e;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// B19-d — COMPRESSION. A captured scene is millions of Gaussians × 14 floats = far too big to ship. The frontier route
// (Self-Organizing Gaussians, Morgenstern 2024): REORDER the Gaussians for LOCALITY so neighbouring entries have
// similar attributes (smooth attribute "planes"), then QUANTISE — a smooth, low-entropy signal an image codec (our own
// HDR codec) compresses hard. This file provides the two on-device primitives: a Morton (Z-order) key from the
// position (sort by it to get spatial locality — reuse the KV radix sort), and a K-bit quantise/dequantise codec.

// MORTON: per Gaussian, quantise its position to 10 bits/axis over the scene bounds and interleave → a 30-bit Z-order
// key; the index rides as the KV-sort payload. Sorting by this key lays spatially-near Gaussians next to each other.
//   b0 gaussians (F32, 14/g)  b1 bounds (F32, 6: [minX minY minZ maxX maxY maxZ])  b2 keys (U32)  b3 index (U32)
struct GsplatMortonConfig
{
    int local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat_morton_kernel(KGraph& g, const GsplatMortonConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  shl = [&](int a, crd::u32 s) { return g.binary(KOp::Shl, a, cu(s)); };
    const auto  ba  = [&](int a, crd::u32 m) { return g.binary(KOp::BitAnd, a, cu(m)); };
    const auto  bo  = [&](int a, int b) { return g.binary(KOp::BitOr, a, b); };
    // interleave a 10-bit value with two zero bits between each bit (part-1-by-2)
    const auto part = [&](int x0) {
        int x = ba(x0, 0x000003FFU);
        x = ba(bo(x, shl(x, 16)), 0xFF0000FFU);
        x = ba(bo(x, shl(x, 8)), 0x0300F00FU);
        x = ba(bo(x, shl(x, 4)), 0x030C30C3U);
        x = ba(bo(x, shl(x, 2)), 0x09249249U);
        return x;
    };

    const int gb   = g.buffer_decl(DType::F32, 0, 0, false);
    const int bd_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int key_b = g.buffer_decl(DType::U32, 0, 2, true);
    const int ordidx_b = g.buffer_decl(DType::U32, 0, 3, true); // the index payload the Morton sort carries
    const int tid  = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                          g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const auto bl = [&](int k) { const int v = g.buffer_load(bd_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int mnx = bl(0); const int mny = bl(1); const int mnz = bl(2);
    const int mxx = bl(3); const int mxy = bl(4); const int mxz = bl(5);
    const int base = g.binary(KOp::Mul, tid, cu(14U));
    const auto q10 = [&](int val, int lo, int hi) {
        const int t01 = g.binary(KOp::Max, g.binary(KOp::Min, dv(g, sub(g, val, lo), g.binary(KOp::Max, sub(g, hi, lo), ks(1.0e-12))), ks(1.0)), ks(0.0));
        return g.cast(g.unary(KOp::Floor, mul(g, t01, ks(1023.0))), DType::U32);
    };
    const int qx = q10(g.buffer_load(gb, base), mnx, mxx);
    const int qy = q10(g.buffer_load(gb, g.binary(KOp::Add, base, cu(1U))), mny, mxy);
    const int qz = q10(g.buffer_load(gb, g.binary(KOp::Add, base, cu(2U))), mnz, mxz);
    const int morton = bo(bo(part(qx), shl(part(qy), 1)), shl(part(qz), 2));
    g.stmt_buffer_store(key_b, tid, morton);
    g.stmt_buffer_store(ordidx_b, tid, g.cast(tid, DType::U32));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// QUANTISE: per Gaussian, map each of `natt` attributes to a K-bit integer code over its [min,max] range. Codes stored
// as F32 (integer-valued) for the oracle; a real codec packs them (32/bits× smaller) and entropy-codes the smooth planes.
//   b0 gaussians (F32, natt/g)  b1 ranges (F32, 2·natt: [min0 max0 …])  b2 codes (F32, natt/g)
struct GsplatQuantizeConfig
{
    int natt       = 14;
    int bits       = 12;
    int local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat_quantize_kernel(KGraph& g, const GsplatQuantizeConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const double levels = static_cast<double>((1U << static_cast<crd::u32>(cfg.bits)) - 1U);

    const int gb  = g.buffer_decl(DType::F32, 0, 0, false);
    const int rb  = g.buffer_decl(DType::F32, 0, 1, false);
    const int qb  = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                         g.builtin(KBuiltin::LocalInvocationIndex));
    const int mark = g.kernel_stmt_mark();
    const int base = g.binary(KOp::Mul, tid, cu(static_cast<crd::u32>(cfg.natt)));
    const int loop = g.stmt_for_begin(cu(static_cast<crd::u32>(cfg.natt)));
    const int k    = g.kernel_loop_var(loop);
    const int gi   = g.binary(KOp::Add, base, k);
    const int x    = g.buffer_load(gb, gi);
    const int lo   = g.buffer_load(rb, g.binary(KOp::Mul, k, cu(2U)));
    const int hi   = g.buffer_load(rb, g.binary(KOp::Add, g.binary(KOp::Mul, k, cu(2U)), cu(1U)));
    g.stmt_materialize(x); g.stmt_materialize(lo); g.stmt_materialize(hi);
    const int t01 = g.binary(KOp::Max, g.binary(KOp::Min, dv(g, sub(g, x, lo), g.binary(KOp::Max, sub(g, hi, lo), ks(1.0e-12))), ks(1.0)), ks(0.0));
    const int q   = g.unary(KOp::Round, mul(g, t01, ks(levels))); // nearest code
    g.stmt_buffer_store(qb, gi, q);
    g.stmt_for_end(loop);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// DEQUANTISE: codes → reconstructed attributes.  x' = min + code/levels · (max − min).
//   b0 codes (F32, natt/g)  b1 ranges (F32, 2·natt)  b2 out (F32, natt/g)
[[nodiscard]] inline KEntry build_gsplat_dequantize_kernel(KGraph& g, const GsplatQuantizeConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const double levels = static_cast<double>((1U << static_cast<crd::u32>(cfg.bits)) - 1U);

    const int qb  = g.buffer_decl(DType::F32, 0, 0, false);
    const int rb  = g.buffer_decl(DType::F32, 0, 1, false);
    const int ob  = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                         g.builtin(KBuiltin::LocalInvocationIndex));
    const int mark = g.kernel_stmt_mark();
    const int base = g.binary(KOp::Mul, tid, cu(static_cast<crd::u32>(cfg.natt)));
    const int loop = g.stmt_for_begin(cu(static_cast<crd::u32>(cfg.natt)));
    const int k    = g.kernel_loop_var(loop);
    const int gi   = g.binary(KOp::Add, base, k);
    const int q    = g.buffer_load(qb, gi);
    const int lo   = g.buffer_load(rb, g.binary(KOp::Mul, k, cu(2U)));
    const int hi   = g.buffer_load(rb, g.binary(KOp::Add, g.binary(KOp::Mul, k, cu(2U)), cu(1U)));
    g.stmt_materialize(q); g.stmt_materialize(lo); g.stmt_materialize(hi);
    const int xr = add(g, lo, mul(g, dv(g, q, ks(levels)), sub(g, hi, lo)));
    g.stmt_buffer_store(ob, gi, xr);
    g.stmt_for_end(loop);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// B19-f — DIFFERENTIABLE TRAINING. 3DGS is FIT to images by gradient descent: render, compare to the photo, and push
// every Gaussian's parameters down the gradient of the photometric loss. This is what makes an engine a CAPTURE-to-
// render tool, not just a viewer. Here is the training core for an isotropic screen Gaussian — the FORWARD splat and
// the exact BACKWARD gradients of L = Σ(C−target)² w.r.t. its position, scale, opacity and colour. The gradients are
// FD-verified and an SGD loop fits the Gaussian to a target. (The multi-splat composite backward extends this with a
// back-to-front suffix accumulation; the single-splat core proves the autodiff-training path end to end.)
//   Params (5): [μx μy · s · opacity · colour]

struct GsplatDiffConfig
{
    int width      = 64;
    int height     = 64;
    int local_size = 64;
};

// FORWARD: C_p = colour · opacity · exp(−½·((px−μx)²+(py−μy)²)/s²). One thread per pixel.
//   b0 params (F32,5)  b1 out (F32, 1/pixel)
[[nodiscard]] inline KEntry build_gsplat_diff_forward_kernel(KGraph& g, const GsplatDiffConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int par_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                          g.builtin(KBuiltin::LocalInvocationIndex));
    const int mark = g.kernel_stmt_mark();
    const int w    = cu(static_cast<crd::u32>(cfg.width));
    const int pxc  = add(g, g.cast(g.binary(KOp::Mod, tid, w), DType::F32), ks(0.5));
    const int pyc  = add(g, g.cast(g.binary(KOp::Div, tid, w), DType::F32), ks(0.5));
    const auto pl  = [&](int k) { const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int mux = pl(0); const int muy = pl(1); const int s = pl(2); const int op = pl(3); const int col = pl(4);
    const int dxp = sub(g, pxc, mux);
    const int dyp = sub(g, pyc, muy);
    const int s2  = g.binary(KOp::Max, sq(g, s), ks(1.0e-9));
    const int e   = g.unary(KOp::Exp, mul(g, ks(-0.5), dv(g, add(g, sq(g, dxp), sq(g, dyp)), s2)));
    g.stmt_buffer_store(out_b, tid, mul(g, col, mul(g, op, e)));

    KEntry e2;
    e2.stage             = KStage::Compute;
    e2.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e2.kernel_body_begin = mark;
    e2.kernel_body_count = g.stmt_count() - mark;
    return e2;
}

// BACKWARD: dL/d(params) of L = Σ_pixels (C_p − target_p)², summed over the image. One thread reduces over all pixels
// (accumulating the 5 gradients in the output buffer — the same RMW pattern the render composite uses for a loop sum).
//   b0 params (F32,5)  b1 target (F32, 1/pixel)  b2 grad (F32,5, write)
[[nodiscard]] inline KEntry build_gsplat_diff_backward_kernel(KGraph& g, const GsplatDiffConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int par_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int tgt_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int grd_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int mark  = g.kernel_stmt_mark();
    const auto pl   = [&](int k) { const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int mux = pl(0); const int muy = pl(1); const int s = pl(2); const int op = pl(3); const int col = pl(4);
    const int s2  = g.binary(KOp::Max, sq(g, s), ks(1.0e-9));
    const int s3  = g.binary(KOp::Max, mul(g, s2, s), ks(1.0e-9));
    for (int k = 0; k < 5; ++k) { g.stmt_buffer_store(grd_b, cu(static_cast<crd::u32>(k)), ks(0.0)); }

    const int loop = g.stmt_for_begin(cu(static_cast<crd::u32>(cfg.width * cfg.height)));
    const int q    = g.kernel_loop_var(loop);
    const int pxc  = add(g, g.cast(g.binary(KOp::Mod, q, cu(static_cast<crd::u32>(cfg.width))), DType::F32), ks(0.5));
    const int pyc  = add(g, g.cast(g.binary(KOp::Div, q, cu(static_cast<crd::u32>(cfg.width))), DType::F32), ks(0.5));
    const int dxp  = sub(g, pxc, mux);
    const int dyp  = sub(g, pyc, muy);
    const int r2   = add(g, sq(g, dxp), sq(g, dyp));
    const int e    = g.unary(KOp::Exp, mul(g, ks(-0.5), dv(g, r2, s2)));
    const int a    = mul(g, op, e);
    const int cval = mul(g, col, a);
    const int tval = g.buffer_load(tgt_b, q);
    g.stmt_materialize(tval);
    const int diff = mul(g, ks(2.0), sub(g, cval, tval)); // dL/dC_p
    const int coe  = mul(g, diff, mul(g, col, op));       // dL/dC · colour·opacity (shared factor for μ/s)
    // per-pixel contributions
    const int d_col = mul(g, diff, a);
    const int d_op  = mul(g, diff, mul(g, col, e));
    const int d_mux = mul(g, coe, mul(g, e, dv(g, dxp, s2)));
    const int d_muy = mul(g, coe, mul(g, e, dv(g, dyp, s2)));
    const int d_s   = mul(g, coe, mul(g, e, dv(g, r2, s3)));
    const auto acc  = [&](int k, int val) {
        const int idx = cu(static_cast<crd::u32>(k));
        const int old = g.buffer_load(grd_b, idx);
        g.stmt_materialize(old);
        g.stmt_buffer_store(grd_b, idx, add(g, old, val));
    };
    acc(0, d_mux); acc(1, d_muy); acc(2, d_s); acc(3, d_op); acc(4, d_col);
    g.stmt_for_end(loop);

    KEntry e2;
    e2.stage             = KStage::Compute;
    e2.local_size[0]     = 1U;
    e2.kernel_body_begin = mark;
    e2.kernel_body_count = g.stmt_count() - mark;
    return e2;
}

// SGD STEP: params -= lr · grad (one thread over the 5 params). b0 params (F32,5, rw)  b1 grad (F32,5)  b2 lr (F32,1)
[[nodiscard]] inline KEntry build_gsplat_sgd_step_kernel(KGraph& g, int nparam = 5)
{
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const int par_b = g.buffer_decl(DType::F32, 0, 0, true);
    const int grd_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int lr_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int tid   = g.builtin(KBuiltin::LocalInvocationIndex);
    const int mark  = g.kernel_stmt_mark();
    const int lr    = g.buffer_load(lr_b, cu(0U));
    const int p     = g.buffer_load(par_b, tid);
    const int gr    = g.buffer_load(grd_b, tid);
    g.stmt_materialize(lr); g.stmt_materialize(p); g.stmt_materialize(gr);
    g.stmt_buffer_store(par_b, tid, g.binary(KOp::Sub, p, g.binary(KOp::Mul, lr, gr)));
    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(nparam);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::gsplat
