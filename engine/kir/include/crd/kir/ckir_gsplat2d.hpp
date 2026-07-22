#pragma once

// ckir_gsplat2d.hpp — D-007 B19-c: 2D GAUSSIAN SPLATTING (Huang et al., SIGGRAPH 2024) — the geometrically-accurate
// surfel primitive. Where 3DGS (ckir_gsplat.hpp) is a cloud of 3D ellipsoids projected by the EWA splat, 2DGS is a cloud
// of oriented flat DISKS (surfels) and renders by an EXACT RAY–SURFEL INTERSECTION. Two payoffs the ellipsoid can't give:
//   • VIEW-CONSISTENT splatting — a flat disk has no perspective-dependent thickness, so the same splat looks identical
//     from every angle (3DGS's projected footprint changes with the view, which is what makes it multi-view-inconsistent).
//   • ACCURATE SURFACE GEOMETRY — the disk IS the surface, so every pixel has a well-defined depth (the ray parameter of
//     the intersection) and a well-defined normal (the disk's own normal). That depth+normal G-buffer is the input a mesh
//     extractor (TSDF fusion / Poisson) consumes — the bridge from captured radiance fields into the B1 mesh/material path.
//
// A 2D Gaussian carries: centre μ(3), two per-axis scales (s_u, s_v), a rotation quaternion q(4) whose first two columns
// are the disk's tangent axes t_u, t_v and whose third column is the surfel normal t_w = t_u×t_v, opacity α(1), and SH
// degree-0 colour(3). Everything is authored in CKIR (scalar compute tier ⇒ component-wise maths), so it lowers to all
// five backends. The intersection is solved in VIEW space (matching the 3DGS camera layout: R,t,fx,fy,cx,cy), which avoids
// building a separate 4×4 clip matrix: a pixel's view-space ray is d=((px−cx)/fx,(py−cy)/fy,1), and the intersection is the
// (u,v,λ) solving  view_centre + u·(s_u·R·t_u) + v·(s_v·R·t_v) = λ·d  — a 3×3 system per surfel per pixel (Cramer's rule).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_gsplat.hpp> // reuse the scalar detail helpers (add/mul/sub/dv/sq/safe_sqrt) + kShC0

namespace crd::kir::gsplat
{

// ── 2DGS PROJECT: per-surfel preprocess. One thread per surfel. Precomputes the view-space quantities the render kernel
//    needs so the per-pixel intersection is a pure 3×3 solve.
//
// Buffers:
//   b0 surfels (F32, 13 / surfel): [μx μy μz · su sv · qx qy qz qw · opacity · shR shG shB]
//   b1 camera  (F32, 20): [R00..R22 (row-major 3×3 view rotation) · tx ty tz · fx fy cx cy · near · imgW imgH]
//   b2 out     (F32, 19 / surfel): [vcx vcy vcz · Ax Ay Az · Bx By Bz · Nx Ny Nz · depth · colR colG colB · opacity · radius · valid]
//     where A = s_u·(R·t_u), B = s_v·(R·t_v) are the view-space tangent axes (scaled), N = normalize(R·t_w) the view normal.
struct Gsplat2dProjectConfig
{
    double near_plane = 0.2;
    int    local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat2d_project_kernel(KGraph& g, const Gsplat2dProjectConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int surf_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int cam_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const auto cl  = [&](int k) {
        const int v = g.buffer_load(cam_b, cu(static_cast<crd::u32>(k)));
        g.stmt_materialize(v);
        return v;
    };
    const int base = g.binary(KOp::Mul, tid, cu(13U));
    const auto gl  = [&](int k) {
        const int v = g.buffer_load(surf_b, g.binary(KOp::Add, base, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };

    // camera
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

    // surfel
    const int mux  = gl(0);
    const int muy  = gl(1);
    const int muz  = gl(2);
    const int su   = gl(3);
    const int sv   = gl(4);
    const int qx   = gl(5);
    const int qy   = gl(6);
    const int qz   = gl(7);
    const int qw   = gl(8);
    const int opac = gl(9);
    const int shr  = gl(10);
    const int shg  = gl(11);
    const int shb  = gl(12);

    // dot of camera row r with a world vector (wx,wy,wz)
    const auto rdot = [&](int a0, int a1, int a2, int wx, int wy, int wz) {
        return add(g, add(g, mul(g, a0, wx), mul(g, a1, wy)), mul(g, a2, wz));
    };

    // R_q columns: t_u = col0, t_v = col1, t_w = col2 (the surfel normal), from a normalised quaternion.
    const int tux = sub(g, ks(1.0), mul(g, ks(2.0), add(g, sq(g, qy), sq(g, qz))));
    const int tuy = mul(g, ks(2.0), add(g, mul(g, qx, qy), mul(g, qw, qz)));
    const int tuz = mul(g, ks(2.0), sub(g, mul(g, qx, qz), mul(g, qw, qy)));
    const int tvx = mul(g, ks(2.0), sub(g, mul(g, qx, qy), mul(g, qw, qz)));
    const int tvy = sub(g, ks(1.0), mul(g, ks(2.0), add(g, sq(g, qx), sq(g, qz))));
    const int tvz = mul(g, ks(2.0), add(g, mul(g, qy, qz), mul(g, qw, qx)));
    const int twx = mul(g, ks(2.0), add(g, mul(g, qx, qz), mul(g, qw, qy)));
    const int twy = mul(g, ks(2.0), sub(g, mul(g, qy, qz), mul(g, qw, qx)));
    const int twz = sub(g, ks(1.0), mul(g, ks(2.0), add(g, sq(g, qx), sq(g, qy))));

    // view-space centre  vc = R·μ + t
    const int vcx = add(g, rdot(r00, r01, r02, mux, muy, muz), tx);
    const int vcy = add(g, rdot(r10, r11, r12, mux, muy, muz), ty);
    const int vcz = add(g, rdot(r20, r21, r22, mux, muy, muz), tz);
    const int vzs = mx(vcz, ks(1.0e-6));
    const int inv = dv(g, ks(1.0), vzs);

    // view-space tangent axes (scaled):  A = s_u·(R·t_u),  B = s_v·(R·t_v)
    const int aux = mul(g, su, rdot(r00, r01, r02, tux, tuy, tuz));
    const int auy = mul(g, su, rdot(r10, r11, r12, tux, tuy, tuz));
    const int auz = mul(g, su, rdot(r20, r21, r22, tux, tuy, tuz));
    const int bvx = mul(g, sv, rdot(r00, r01, r02, tvx, tvy, tvz));
    const int bvy = mul(g, sv, rdot(r10, r11, r12, tvx, tvy, tvz));
    const int bvz = mul(g, sv, rdot(r20, r21, r22, tvx, tvy, tvz));

    // view-space normal  N = normalize(R·t_w)
    const int nx0 = rdot(r00, r01, r02, twx, twy, twz);
    const int ny0 = rdot(r10, r11, r12, twx, twy, twz);
    const int nz0 = rdot(r20, r21, r22, twx, twy, twz);
    const int nln = mx(safe_sqrt(g, add(g, add(g, sq(g, nx0), sq(g, ny0)), sq(g, nz0))), ks(1.0e-12));
    const int ninv = dv(g, ks(1.0), nln);
    const int nx  = mul(g, nx0, ninv);
    const int ny  = mul(g, ny0, ninv);
    const int nz  = mul(g, nz0, ninv);

    // screen radius (approx, for later tiling): 3·f·max(s_u,s_v)/z
    const int radius = mul(g, ks(3.0), mul(g, mul(g, mx(fx, fy), mx(g.unary(KOp::Abs, su), g.unary(KOp::Abs, sv))), inv));

    // SH degree-0 colour
    const int col_r = mx(add(g, ks(0.5), mul(g, ks(kShC0), shr)), ks(0.0));
    const int col_g = mx(add(g, ks(0.5), mul(g, ks(kShC0), shg)), ks(0.0));
    const int col_b = mx(add(g, ks(0.5), mul(g, ks(kShC0), shb)), ks(0.0));

    const int valid = g.select(g.binary(KOp::CmpGt, vcz, ks(cfg.near_plane)), ks(1.0), ks(0.0));

    const int ob = g.binary(KOp::Mul, tid, cu(19U));
    const auto st = [&](int k, int v) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(static_cast<crd::u32>(k))), v); };
    st(0, vcx); st(1, vcy); st(2, vcz);
    st(3, aux); st(4, auy); st(5, auz);
    st(6, bvx); st(7, bvy); st(8, bvz);
    st(9, nx); st(10, ny); st(11, nz);
    st(12, vcz);
    st(13, col_r); st(14, col_g); st(15, col_b);
    st(16, opac);
    st(17, radius);
    st(18, valid);
    (void)cx; (void)cy;

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── 2DGS RENDER: one thread per pixel, brute front-to-back composite over the DEPTH-SORTED prepared surfels. For each
//    surfel it solves the ray–surfel intersection (u,v,λ) by Cramer's rule on the 3×3 system
//        u·A + v·B − λ·d = −vc,   d = ((px−cx)/fx, (py−cy)/fy, 1),
//    evaluates G = exp(−½(u²+v²)) with a 3σ (u²+v² < 9) footprint cull, and over-composites colour PLUS the α-weighted
//    depth (λ) and normal — the geometry G-buffer a mesh extractor consumes. Output per pixel (8 floats):
//        [R G B T · depthSum · Nx Ny Nz]  where the surface depth = depthSum/(1−T) and the surface normal = Σw·N normalised.
//
// Buffers:
//   b0 surfels (F32, 19 / surfel, DEPTH-SORTED)  ·  b1 camera (F32, 20)  ·  b2 params (F32, 5: [count · bgR bgG bgB · alphaMin])
//   b3 out (F32, 8 / pixel)
struct Gsplat2dRenderConfig
{
    int    width      = 512;
    int    height     = 512;
    int    max_splats = 256;
    int    local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat2d_render_kernel(KGraph& g, const Gsplat2dRenderConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int surf_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int cam_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int par_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 3, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int w    = cu(static_cast<crd::u32>(cfg.width));
    const int px   = g.cast(g.binary(KOp::Mod, tid, w), DType::F32);
    const int py   = g.cast(g.binary(KOp::Div, tid, w), DType::F32);
    const int pxc  = add(g, px, ks(0.5));
    const int pyc  = add(g, py, ks(0.5));

    const auto cl = [&](int k) {
        const int v = g.buffer_load(cam_b, cu(static_cast<crd::u32>(k)));
        g.stmt_materialize(v);
        return v;
    };
    const int fx   = cl(12);
    const int fy   = cl(13);
    const int cx   = cl(14);
    const int cy   = cl(15);
    const int near = cl(16);
    const int dx   = dv(g, sub(g, pxc, cx), fx); // view-space ray direction (dz = 1)
    const int dy   = dv(g, sub(g, pyc, cy), fy);
    g.stmt_materialize(dx);
    g.stmt_materialize(dy);

    const auto pl = [&](int k) {
        const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k)));
        g.stmt_materialize(v);
        return v;
    };
    const int count = g.cast(pl(0), DType::U32);
    const int bg_r  = pl(1);
    const int bg_g  = pl(2);
    const int bg_b  = pl(3);
    const int amin  = pl(4);

    const int p8 = g.binary(KOp::Mul, tid, cu(8U));
    const auto ost = [&](int k, int v) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, p8, cu(static_cast<crd::u32>(k))), v); };
    ost(0, ks(0.0)); ost(1, ks(0.0)); ost(2, ks(0.0));
    ost(3, ks(1.0)); // transmittance
    ost(4, ks(0.0)); // depth sum
    ost(5, ks(0.0)); ost(6, ks(0.0)); ost(7, ks(0.0)); // normal sum

    const int loop = g.stmt_for_begin(count);
    const int si   = g.kernel_loop_var(loop);
    const int sb   = g.binary(KOp::Mul, si, cu(19U));
    const auto jl  = [&](int k) {
        const int v = g.buffer_load(surf_b, g.binary(KOp::Add, sb, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int vcx = jl(0);
    const int vcy = jl(1);
    const int vcz = jl(2);
    const int aux = jl(3);
    const int auy = jl(4);
    const int auz = jl(5);
    const int bvx = jl(6);
    const int bvy = jl(7);
    const int bvz = jl(8);
    const int snx = jl(9);
    const int sny = jl(10);
    const int snz = jl(11);
    const int colr = jl(13);
    const int colg = jl(14);
    const int colb = jl(15);
    const int sop  = jl(16);
    const int sval = jl(18);

    // Cramer's rule on [A | B | −d]·(u,v,λ) = −vc.
    const int bxd_x = sub(g, bvy, mul(g, bvz, dy));          // B × d, with d = (dx,dy,1)
    const int bxd_y = sub(g, mul(g, bvz, dx), bvx);
    const int bxd_z = sub(g, mul(g, bvx, dy), mul(g, bvy, dx));
    const int den   = add(g, add(g, mul(g, aux, bxd_x), mul(g, auy, bxd_y)), mul(g, auz, bxd_z)); // A·(B×d)
    const int den_abs = g.unary(KOp::Abs, den);
    const int nondeg  = g.binary(KOp::CmpGt, den_abs, ks(1.0e-12));
    const int den_s   = g.select(nondeg, den, ks(1.0)); // divide-safe (masked out below when degenerate)
    const int inv_den = dv(g, ks(1.0), den_s);

    const int vc_bxd = add(g, add(g, mul(g, vcx, bxd_x), mul(g, vcy, bxd_y)), mul(g, vcz, bxd_z));
    const int uu = mul(g, g.unary(KOp::Neg, vc_bxd), inv_den); // u
    const int vxd_x = sub(g, vcy, mul(g, vcz, dy));           // vc × d
    const int vxd_y = sub(g, mul(g, vcz, dx), vcx);
    const int vxd_z = sub(g, mul(g, vcx, dy), mul(g, vcy, dx));
    const int a_vxd = add(g, add(g, mul(g, aux, vxd_x), mul(g, auy, vxd_y)), mul(g, auz, vxd_z));
    const int vv = mul(g, g.unary(KOp::Neg, a_vxd), inv_den); // v
    const int bxv_x = sub(g, mul(g, bvy, vcz), mul(g, bvz, vcy)); // B × vc
    const int bxv_y = sub(g, mul(g, bvz, vcx), mul(g, bvx, vcz));
    const int bxv_z = sub(g, mul(g, bvx, vcy), mul(g, bvy, vcx));
    const int a_bxv = add(g, add(g, mul(g, aux, bxv_x), mul(g, auy, bxv_y)), mul(g, auz, bxv_z));
    const int lam = mul(g, a_bxv, inv_den); // λ = view-space depth of the intersection (d.z = 1)

    const int r2   = add(g, sq(g, uu), sq(g, vv));
    const int gval = g.unary(KOp::Exp, mul(g, ks(-0.5), r2));
    const int aeff = g.binary(KOp::Min, mul(g, sop, gval), ks(0.99));
    const int keep = g.binary(KOp::BitAnd,
                              g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, sval, ks(0.5)), nondeg),
                              g.binary(KOp::BitAnd, g.binary(KOp::CmpLt, r2, ks(9.0)),
                                       g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, lam, near), g.binary(KOp::CmpGe, aeff, amin))));
    const int a = g.select(keep, aeff, ks(0.0));

    const int t_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(3U)));
    g.stmt_materialize(t_old);
    const int wgt = mul(g, a, t_old);
    const int r_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(0U)));
    const int g_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(1U)));
    const int b_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(2U)));
    const int d_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(4U)));
    const int nx_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(5U)));
    const int ny_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(6U)));
    const int nz_old = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(7U)));
    g.stmt_materialize(r_old); g.stmt_materialize(g_old); g.stmt_materialize(b_old);
    g.stmt_materialize(d_old); g.stmt_materialize(nx_old); g.stmt_materialize(ny_old); g.stmt_materialize(nz_old);
    ost(0, add(g, r_old, mul(g, colr, wgt)));
    ost(1, add(g, g_old, mul(g, colg, wgt)));
    ost(2, add(g, b_old, mul(g, colb, wgt)));
    ost(4, add(g, d_old, mul(g, lam, wgt)));
    ost(5, add(g, nx_old, mul(g, snx, wgt)));
    ost(6, add(g, ny_old, mul(g, sny, wgt)));
    ost(7, add(g, nz_old, mul(g, snz, wgt)));
    ost(3, mul(g, t_old, sub(g, ks(1.0), a)));
    g.stmt_for_end(loop);

    const int tf = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(3U)));
    g.stmt_materialize(tf);
    const int rf = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(0U)));
    const int gf = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(1U)));
    const int bf = g.buffer_load(out_b, g.binary(KOp::Add, p8, cu(2U)));
    g.stmt_materialize(rf); g.stmt_materialize(gf); g.stmt_materialize(bf);
    ost(0, add(g, rf, mul(g, bg_r, tf)));
    ost(1, add(g, gf, mul(g, bg_g, tf)));
    ost(2, add(g, bf, mul(g, bg_b, tf)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    (void)cfg.height;
    return e;
}

// ── B19-e: RELIGHTABLE 2DGS render. Base 3DGS/2DGS bakes lighting into the SH colour, so a captured scene can only be
//    replayed under its capture illumination. Here each surfel carries an ALBEDO (its colour slot) and its intrinsic
//    NORMAL, and the pixel is shaded with a physically-based BRDF (Lambert diffuse + GGX/Cook-Torrance specular) under a
//    DIRECTIONAL LIGHT — so captured content responds to new lighting, the prerequisite for dropping it into a dynamic
//    scene. The surfel's exact ray-intersection normal (not a soft ellipsoid blob) makes the specular meaningful.
//   b0 surfels (19/surfel, DEPTH-SORTED)  b1 camera(20)
//   b2 params (13): [count · bgR bgG bgB · Lx Ly Lz (view-space light dir) · lcR lcG lcB · ambient · roughness · alphaMin]
//   b3 out (F32, 4/pixel RGBA)
struct Gsplat2dRelightConfig
{
    int width      = 512;
    int height     = 512;
    int max_splats = 256;
    int local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat2d_relight_render_kernel(KGraph& g, const Gsplat2dRelightConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  mn  = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto  mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto  dot3 = [&](int ax, int ay, int az, int bx, int by, int bz) { return add(g, add(g, mul(g, ax, bx), mul(g, ay, by)), mul(g, az, bz)); };

    const int surf_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int cam_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int par_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 3, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int w    = cu(static_cast<crd::u32>(cfg.width));
    const int px   = g.cast(g.binary(KOp::Mod, tid, w), DType::F32);
    const int py   = g.cast(g.binary(KOp::Div, tid, w), DType::F32);
    const int pxc  = add(g, px, ks(0.5));
    const int pyc  = add(g, py, ks(0.5));

    const auto cl = [&](int k) { const int v = g.buffer_load(cam_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int fx   = cl(12);
    const int fy   = cl(13);
    const int cx   = cl(14);
    const int cy   = cl(15);
    const int near = cl(16);
    const int dx   = dv(g, sub(g, pxc, cx), fx);
    const int dy   = dv(g, sub(g, pyc, cy), fy);
    g.stmt_materialize(dx);
    g.stmt_materialize(dy);
    // view direction v = −d / |d| (surface → camera at the origin)
    const int dlen = safe_sqrt(g, add(g, add(g, sq(g, dx), sq(g, dy)), ks(1.0)));
    const int idl  = dv(g, ks(1.0), dlen);
    const int vx   = mul(g, g.unary(KOp::Neg, dx), idl);
    const int vy   = mul(g, g.unary(KOp::Neg, dy), idl);
    const int vz   = mul(g, g.unary(KOp::Neg, ks(1.0)), idl);
    g.stmt_materialize(vx); g.stmt_materialize(vy); g.stmt_materialize(vz);

    const auto pl = [&](int k) { const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int count = g.cast(pl(0), DType::U32);
    const int bg_r  = pl(1);
    const int bg_g  = pl(2);
    const int bg_b  = pl(3);
    const int lrx0  = pl(4);
    const int lry0  = pl(5);
    const int lrz0  = pl(6);
    const int lcr   = pl(7);
    const int lcg   = pl(8);
    const int lcb   = pl(9);
    const int ambient = pl(10);
    const int rough   = pl(11);
    const int amin    = pl(12);
    // normalise the light direction (points surface → light)
    const int llen = safe_sqrt(g, add(g, add(g, sq(g, lrx0), sq(g, lry0)), sq(g, lrz0)));
    const int ill  = dv(g, ks(1.0), mx(llen, ks(1.0e-9)));
    const int lx   = mul(g, lrx0, ill);
    const int ly   = mul(g, lry0, ill);
    const int lz   = mul(g, lrz0, ill);
    g.stmt_materialize(lx); g.stmt_materialize(ly); g.stmt_materialize(lz);
    // GGX parameters
    const int a    = mx(mul(g, rough, rough), ks(1.0e-3)); // GGX alpha = roughness²
    const int a2   = mul(g, a, a);
    const int kdir = mul(g, sq(g, add(g, rough, ks(1.0))), ks(0.125)); // Smith k = (r+1)²/8
    g.stmt_materialize(a2); g.stmt_materialize(kdir);

    const int p4 = g.binary(KOp::Mul, tid, cu(4U));
    const auto ost = [&](int k, int v) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(static_cast<crd::u32>(k))), v); };
    ost(0, ks(0.0)); ost(1, ks(0.0)); ost(2, ks(0.0)); ost(3, ks(1.0));

    const int loop = g.stmt_for_begin(count);
    const int si   = g.kernel_loop_var(loop);
    const int sb   = g.binary(KOp::Mul, si, cu(19U));
    const auto jl  = [&](int k) { const int v = g.buffer_load(surf_b, g.binary(KOp::Add, sb, cu(static_cast<crd::u32>(k)))); g.stmt_materialize(v); return v; };
    const int vcx = jl(0);
    const int vcy = jl(1);
    const int vcz = jl(2);
    const int aux = jl(3);
    const int auy = jl(4);
    const int auz = jl(5);
    const int bvx = jl(6);
    const int bvy = jl(7);
    const int bvz = jl(8);
    const int snx = jl(9);
    const int sny = jl(10);
    const int snz = jl(11);
    const int alr = jl(13); // albedo
    const int alg = jl(14);
    const int alb = jl(15);
    const int sop = jl(16);
    const int sval = jl(18);

    // ray–surfel intersection (Cramer's rule, as the base 2DGS render)
    const int bxd_x = sub(g, bvy, mul(g, bvz, dy));
    const int bxd_y = sub(g, mul(g, bvz, dx), bvx);
    const int bxd_z = sub(g, mul(g, bvx, dy), mul(g, bvy, dx));
    const int den   = add(g, add(g, mul(g, aux, bxd_x), mul(g, auy, bxd_y)), mul(g, auz, bxd_z));
    const int nondeg = g.binary(KOp::CmpGt, g.unary(KOp::Abs, den), ks(1.0e-12));
    const int inv_den = dv(g, ks(1.0), g.select(nondeg, den, ks(1.0)));
    const int uu = mul(g, g.unary(KOp::Neg, add(g, add(g, mul(g, vcx, bxd_x), mul(g, vcy, bxd_y)), mul(g, vcz, bxd_z))), inv_den);
    const int vxd_x = sub(g, vcy, mul(g, vcz, dy));
    const int vxd_y = sub(g, mul(g, vcz, dx), vcx);
    const int vxd_z = sub(g, mul(g, vcx, dy), mul(g, vcy, dx));
    const int vv = mul(g, g.unary(KOp::Neg, add(g, add(g, mul(g, aux, vxd_x), mul(g, auy, vxd_y)), mul(g, auz, vxd_z))), inv_den);
    const int bxv_x = sub(g, mul(g, bvy, vcz), mul(g, bvz, vcy));
    const int bxv_y = sub(g, mul(g, bvz, vcx), mul(g, bvx, vcz));
    const int bxv_z = sub(g, mul(g, bvx, vcy), mul(g, bvy, vcx));
    const int lam = mul(g, add(g, add(g, mul(g, aux, bxv_x), mul(g, auy, bxv_y)), mul(g, auz, bxv_z)), inv_den);
    const int r2  = add(g, sq(g, uu), sq(g, vv));
    const int gval = g.unary(KOp::Exp, mul(g, ks(-0.5), r2));
    const int aeff = mn(mul(g, sop, gval), ks(0.99));
    const int keep = g.binary(KOp::BitAnd, g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, sval, ks(0.5)), nondeg),
                              g.binary(KOp::BitAnd, g.binary(KOp::CmpLt, r2, ks(9.0)),
                                       g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, lam, near), g.binary(KOp::CmpGe, aeff, amin))));
    const int aa = g.select(keep, aeff, ks(0.0));

    // ── PBR shade: orient the normal toward the camera, Lambert diffuse + Cook-Torrance GGX under the directional light ──
    const int ndv_raw = dot3(snx, sny, snz, vx, vy, vz);
    const int sgn = g.select(g.binary(KOp::CmpLt, ndv_raw, ks(0.0)), ks(-1.0), ks(1.0));
    const int nx = mul(g, sgn, snx);
    const int ny = mul(g, sgn, sny);
    const int nz = mul(g, sgn, snz);
    const int ndv = mx(g.unary(KOp::Abs, ndv_raw), ks(1.0e-4));
    const int ndl = mx(dot3(nx, ny, nz, lx, ly, lz), ks(0.0));
    const int hx0 = add(g, lx, vx);
    const int hy0 = add(g, ly, vy);
    const int hz0 = add(g, lz, vz);
    const int ihl = dv(g, ks(1.0), mx(safe_sqrt(g, add(g, add(g, sq(g, hx0), sq(g, hy0)), sq(g, hz0))), ks(1.0e-6)));
    const int hx = mul(g, hx0, ihl);
    const int hy = mul(g, hy0, ihl);
    const int hz = mul(g, hz0, ihl);
    const int ndh = mx(dot3(nx, ny, nz, hx, hy, hz), ks(0.0));
    const int vdh = mx(dot3(vx, vy, vz, hx, hy, hz), ks(0.0));
    // D (GGX)
    const int dd  = add(g, mul(g, sq(g, ndh), sub(g, a2, ks(1.0))), ks(1.0));
    const int dggx = dv(g, a2, mx(mul(g, ks(3.14159265358979), sq(g, dd)), ks(1.0e-7)));
    // G (Smith, Schlick-GGX)
    const int g1l = dv(g, ndl, mx(add(g, mul(g, ndl, sub(g, ks(1.0), kdir)), kdir), ks(1.0e-6)));
    const int g1v = dv(g, ndv, mx(add(g, mul(g, ndv, sub(g, ks(1.0), kdir)), kdir), ks(1.0e-6)));
    const int gsm = mul(g, g1l, g1v);
    // F (Schlick, F0 = 0.04 dielectric)
    const int om = sub(g, ks(1.0), vdh);
    const int fr = add(g, ks(0.04), mul(g, ks(0.96), mul(g, sq(g, sq(g, om)), om)));
    const int spec = dv(g, mul(g, mul(g, dggx, gsm), fr), mul(g, ks(4.0), ndv)); // Lo_spec / (Li·ndl-cancelled)
    const int kd   = sub(g, ks(1.0), fr);
    // Lo = ((1−F)·albedo·(N·L) + spec)·lightColour + ambient·albedo
    const int shr = add(g, mul(g, add(g, mul(g, mul(g, kd, alr), ndl), spec), lcr), mul(g, ambient, alr));
    const int shg = add(g, mul(g, add(g, mul(g, mul(g, kd, alg), ndl), spec), lcg), mul(g, ambient, alg));
    const int shb = add(g, mul(g, add(g, mul(g, mul(g, kd, alb), ndl), spec), lcb), mul(g, ambient, alb));

    const int t_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(t_old);
    const int wgt = mul(g, aa, t_old);
    const int r_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int g_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int b_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(r_old); g.stmt_materialize(g_old); g.stmt_materialize(b_old);
    ost(0, add(g, r_old, mul(g, shr, wgt)));
    ost(1, add(g, g_old, mul(g, shg, wgt)));
    ost(2, add(g, b_old, mul(g, shb, wgt)));
    ost(3, mul(g, t_old, sub(g, ks(1.0), aa)));
    g.stmt_for_end(loop);

    const int tf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(tf);
    const int rf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int gf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int bf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(rf); g.stmt_materialize(gf); g.stmt_materialize(bf);
    ost(0, add(g, rf, mul(g, bg_r, tf)));
    ost(1, add(g, gf, mul(g, bg_g, tf)));
    ost(2, add(g, bf, mul(g, bg_b, tf)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    (void)cfg.height;
    return e;
}

// ── B19 StopThePop (Radl et al. 2024): PER-PIXEL RESORT. The base render composites a tile's splats in the GLOBAL
//    (per-splat centre) depth order, but the correct order for a given pixel is by each splat's depth AT THAT PIXEL —
//    the ray intersection λ, which varies across a slanted surfel. When those two orders disagree (splats crossing in
//    depth across the frame) the base render composites out of order, and a rotating view flips the global order
//    discretely ⇒ POPPING. This kernel resorts per pixel: it composites the surfels in ascending per-pixel λ via an
//    O(N²) selection (state kept in a per-pixel scratch buffer, since a CKIR `For` carries no register state), so the
//    order is exact at every pixel and the popping is gone.
//   b0 surfels (19/surfel)  b1 camera(20)  b2 params(5: [count · bgR bgG bgB · alphaMin])
//   b3 out (F32, 4/pixel RGBA)  b4 scratch (F32, 4/pixel: [lastLam lastIdx bestLam bestIdx])
struct Gsplat2dResortConfig
{
    int width      = 512;
    int height     = 512;
    int max_splats = 16; // the per-pixel selection runs N² — keep N modest (a tile's cap)
    int local_size = 64;
};

[[nodiscard]] inline KEntry build_gsplat2d_resort_render_kernel(KGraph& g, const Gsplat2dResortConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  mn  = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto  orr = [&](int a, int b) { return g.binary(KOp::BitOr, a, b); };
    const auto  andd = [&](int a, int b) { return g.binary(KOp::BitAnd, a, b); };

    const int surf_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int cam_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int par_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 3, true);
    const int scr_b  = g.buffer_decl(DType::F32, 0, 4, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int w    = cu(static_cast<crd::u32>(cfg.width));
    const int pxc  = add(g, g.cast(g.binary(KOp::Mod, tid, w), DType::F32), ks(0.5));
    const int pyc  = add(g, g.cast(g.binary(KOp::Div, tid, w), DType::F32), ks(0.5));
    const auto cl  = [&](int k) { const int v = g.buffer_load(cam_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int fx = cl(12); const int fy = cl(13); const int cx = cl(14); const int cy = cl(15); const int near = cl(16);
    const int dx = dv(g, sub(g, pxc, cx), fx);
    const int dy = dv(g, sub(g, pyc, cy), fy);
    g.stmt_materialize(dx); g.stmt_materialize(dy);
    const auto pl = [&](int k) { const int v = g.buffer_load(par_b, cu(static_cast<crd::u32>(k))); g.stmt_materialize(v); return v; };
    const int bg_r = pl(1); const int bg_g = pl(2); const int bg_b = pl(3); const int amin = pl(4);

    // per-surfel ray intersection → (λ, keep-mask, effective α, colour). `si` is a runtime index.
    struct Hit { int lam; int keep; int aeff; int cr; int cg; int cb; };
    const auto hit = [&](int si) -> Hit {
        const int sb = g.binary(KOp::Mul, si, cu(19U));
        const auto jl = [&](int k) { const int v = g.buffer_load(surf_b, g.binary(KOp::Add, sb, cu(static_cast<crd::u32>(k)))); g.stmt_materialize(v); return v; };
        const int vcx = jl(0); const int vcy = jl(1); const int vcz = jl(2);
        const int aux = jl(3); const int auy = jl(4); const int auz = jl(5);
        const int bvx = jl(6); const int bvy = jl(7); const int bvz = jl(8);
        const int colr = jl(13); const int colg = jl(14); const int colb = jl(15);
        const int sop = jl(16); const int sval = jl(18);
        const int bxd_x = sub(g, bvy, mul(g, bvz, dy));
        const int bxd_y = sub(g, mul(g, bvz, dx), bvx);
        const int bxd_z = sub(g, mul(g, bvx, dy), mul(g, bvy, dx));
        const int den = add(g, add(g, mul(g, aux, bxd_x), mul(g, auy, bxd_y)), mul(g, auz, bxd_z));
        const int nondeg = g.binary(KOp::CmpGt, g.unary(KOp::Abs, den), ks(1.0e-12));
        const int inv_den = dv(g, ks(1.0), g.select(nondeg, den, ks(1.0)));
        const int uu = mul(g, g.unary(KOp::Neg, add(g, add(g, mul(g, vcx, bxd_x), mul(g, vcy, bxd_y)), mul(g, vcz, bxd_z))), inv_den);
        const int vxd_x = sub(g, vcy, mul(g, vcz, dy));
        const int vxd_y = sub(g, mul(g, vcz, dx), vcx);
        const int vxd_z = sub(g, mul(g, vcx, dy), mul(g, vcy, dx));
        const int vv = mul(g, g.unary(KOp::Neg, add(g, add(g, mul(g, aux, vxd_x), mul(g, auy, vxd_y)), mul(g, auz, vxd_z))), inv_den);
        const int bxv_x = sub(g, mul(g, bvy, vcz), mul(g, bvz, vcy));
        const int bxv_y = sub(g, mul(g, bvz, vcx), mul(g, bvx, vcz));
        const int bxv_z = sub(g, mul(g, bvx, vcy), mul(g, bvy, vcx));
        const int lam = mul(g, add(g, add(g, mul(g, aux, bxv_x), mul(g, auy, bxv_y)), mul(g, auz, bxv_z)), inv_den);
        const int r2 = add(g, sq(g, uu), sq(g, vv));
        const int aeff = mn(mul(g, sop, g.unary(KOp::Exp, mul(g, ks(-0.5), r2))), ks(0.99));
        const int keep = andd(andd(g.binary(KOp::CmpGt, sval, ks(0.5)), nondeg),
                              andd(g.binary(KOp::CmpLt, r2, ks(9.0)), andd(g.binary(KOp::CmpGt, lam, near), g.binary(KOp::CmpGe, aeff, amin))));
        Hit h;
        h.lam = lam; h.keep = keep; h.aeff = aeff; h.cr = colr; h.cg = colg; h.cb = colb;
        // materialise the FLOAT results (used across the select/composite); NEVER the Bool `keep` (no bool temp type).
        g.stmt_materialize(h.lam); g.stmt_materialize(h.aeff);
        g.stmt_materialize(h.cr); g.stmt_materialize(h.cg); g.stmt_materialize(h.cb);
        return h;
    };

    const int nf = ks(static_cast<double>(cfg.max_splats));
    const int p4 = g.binary(KOp::Mul, tid, cu(4U));
    const int s4 = g.binary(KOp::Mul, tid, cu(4U));
    const auto ost = [&](int k, int v) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, p4, cu(static_cast<crd::u32>(k))), v); };
    const auto sst = [&](int k, int v) { g.stmt_buffer_store(scr_b, g.binary(KOp::Add, s4, cu(static_cast<crd::u32>(k))), v); };
    const auto sld = [&](int k) { const int v = g.buffer_load(scr_b, g.binary(KOp::Add, s4, cu(static_cast<crd::u32>(k)))); g.stmt_materialize(v); return v; };
    ost(0, ks(0.0)); ost(1, ks(0.0)); ost(2, ks(0.0)); ost(3, ks(1.0));
    sst(0, ks(-1.0e30)); sst(1, ks(-1.0)); // lastLam, lastIdx

    // outer: pick the k-th nearest (per-pixel λ) surfel and composite it.
    const int ol = g.stmt_for_begin(cu(static_cast<crd::u32>(cfg.max_splats)));
    (void)g.kernel_loop_var(ol);
    sst(2, ks(1.0e30)); sst(3, nf); // reset bestLam, bestIdx for this round
    // inner: scan all surfels for the smallest (λ, idx) strictly greater than (lastLam, lastIdx).
    const int il = g.stmt_for_begin(cu(static_cast<crd::u32>(cfg.max_splats)));
    const int ii = g.cast(g.kernel_loop_var(il), DType::F32);
    const Hit hi = hit(g.cast(ii, DType::U32));
    const int last_lam = sld(0);
    const int last_idx = sld(1);
    const int best_lam = sld(2);
    const int best_idx = sld(3);
    const int gt = orr(g.binary(KOp::CmpGt, hi.lam, last_lam),
                       andd(g.binary(KOp::CmpEq, hi.lam, last_lam), g.binary(KOp::CmpGt, ii, last_idx)));
    const int cand = andd(hi.keep, gt);
    const int lt = orr(g.binary(KOp::CmpLt, hi.lam, best_lam),
                       andd(g.binary(KOp::CmpEq, hi.lam, best_lam), g.binary(KOp::CmpLt, ii, best_idx)));
    const int take = andd(cand, lt);
    sst(2, g.select(take, hi.lam, best_lam));
    sst(3, g.select(take, ii, best_idx));
    g.stmt_for_end(il);

    // composite the selected surfel (if any) and advance lastLam/lastIdx.
    const int bl = sld(2);
    const int bi = sld(3);
    const int cur_last_lam = sld(0);
    const int cur_last_idx = sld(1);
    const int found = g.binary(KOp::CmpLt, bi, nf);
    const Hit hb = hit(g.cast(bi, DType::U32));
    const int a = g.select(andd(found, hb.keep), hb.aeff, ks(0.0));
    const int t_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(t_old);
    const int wgt = mul(g, a, t_old);
    const int r_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int g_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int b_old = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(r_old); g.stmt_materialize(g_old); g.stmt_materialize(b_old);
    ost(0, add(g, r_old, mul(g, hb.cr, wgt)));
    ost(1, add(g, g_old, mul(g, hb.cg, wgt)));
    ost(2, add(g, b_old, mul(g, hb.cb, wgt)));
    ost(3, mul(g, t_old, sub(g, ks(1.0), a)));
    sst(0, g.select(found, bl, cur_last_lam));
    sst(1, g.select(found, bi, cur_last_idx));
    g.stmt_for_end(ol);

    const int tf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(3U)));
    g.stmt_materialize(tf);
    const int rf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(0U)));
    const int gf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(1U)));
    const int bf = g.buffer_load(out_b, g.binary(KOp::Add, p4, cu(2U)));
    g.stmt_materialize(rf); g.stmt_materialize(gf); g.stmt_materialize(bf);
    ost(0, add(g, rf, mul(g, bg_r, tf)));
    ost(1, add(g, gf, mul(g, bg_g, tf)));
    ost(2, add(g, bf, mul(g, bg_b, tf)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    (void)cfg.height; (void)fy;
    return e;
}

} // namespace crd::kir::gsplat
