#pragma once

// ckir_water_render.hpp — the reusable, node-editor-drivable CKIR OCEAN RENDER pass (B16, D-007). Promoted out of the test
// harness (2026-07-16): the engine now owns the displaced-geometry ocean surface so the renderer + the node editor can drive it.
//
// The pass consumes the 4 baked FFT cascades (from `ckir_ocean.hpp`'s spectrum→evolve→IFFT→assemble compute pipeline — each a
// bindless RGBA8 `[nx, nz, height, ½(1−J)]`) and renders a Johanson PROJECTED GRID: a screen-space lattice raycast onto the water
// plane, VERTEX-displaced by the summed cascade heights, then shaded per-pixel. `ocean_projected_vertex` is the ONE geometry
// function; the vertex-pull VS (`build_ocean_displaced_vs`), the mesh-shader fast path (`build_ocean_displaced_mesh`), and their
// HLSL/GLSL emitters all call it, so every path renders pixel-identically. `build_ocean_water_geo_fs` shades the surface (the
// high-frequency chop lives here as a per-pixel mip-filtered normal map — the fix for the "white-noise" look — combined with the
// smooth swell geometry). `OceanCascadeRender` is the whole config: 4 non-harmonic world scales (⇒ LCM tiling ⇒ effectively
// non-repeating), per-cascade geometry / normal / foam weights, and the joint-Jacobian whitecap thresholds.
//
// Portability: pure vertex-pull lowers to GLSL + HLSL today (WGSL/MSL when the bindless-LOD sampling gains a texture_2d_array
// form — WebGPU has no descriptor-array bindless); the mesh path renders on Vulkan + emits valid DX12 HLSL.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp> // nodes::detail::bin (broadcast-safe vec·scalar)
#include <crd/kir/ckir_noise.hpp> // nodes::noise::worley2 / fractal2 (the textured-foam break-up)
#include <crd/kir/ckir_water.hpp> // fresnel_water · ocean_sun_glitter (the surface BRDF helpers)

namespace crd::kir::water
{

// The 4-cascade ocean render config (the production standard — Sea of Thieves / gasgiant). FOUR independent FFT spectra at
// descending world scales, band-limited so each owns a wavelength band. The BIG cascades carry HIGH-amplitude geometry (the
// rolling swell silhouette); the FINE cascades are LOW-amplitude and live mostly in the per-pixel normal (detail, no aliasing).
struct OceanCascadeRender
{
    int    count      = 4;
    double patch[4]   = {237.0, 92.0, 39.0, 16.5}; // world scale per cascade (non-harmonic ⇒ no visible tiling)
    double hmax[4]    = {1.0, 1.0, 1.0, 1.0};       // per-cascade height-decode scale (filled from the bake)
    double geo_w[4]   = {1.0, 0.62, 0.28, 0.10};    // GEOMETRY displacement weight (big waves tall, fine waves flat)
    double nrm_w[4]   = {1.15, 1.0, 0.72, 0.42};    // shading-NORMAL weight (fine detail present but LOW amplitude)
    double foam_w[4]  = {1.0, 1.0, 0.95, 0.75};     // per-cascade fold weight into the joint foam
    double foam_bias  = 0.42;                        // joint-fold threshold for a whitecap
    double foam_scale = 2.1;                         // whitecap ramp steepness
};

// Shared projected-grid CAMERA + LATTICE constants — the ONE source, so the vertex-pull VS + the mesh path use the identical
// camera (eye (0,kEyeH,0), rdir = normalize(ux·kFovx, −kPitch+uy·kFovy, 1)) ⇒ the geometry composites pixel-aligned over the sky.
namespace ocean_grid
{
constexpr double kEyeH  = 5.0;   // eye at (0, kEyeH, 0), looking +z
constexpr double kFovx  = 0.9;   // rdir.x = ux·kFovx
constexpr double kFovy  = 0.60;  // rdir.y = −kPitch + uy·kFovy
constexpr double kPitch = 0.14;  // downward tilt (horizon a touch above centre)
constexpr double kZnear = 1.0;
constexpr double kZfar  = 500.0;
constexpr double kUxmax = 1.4;   // horizontal over-fetch (grid well past the screen edges — no corner gaps after displacement)
constexpr double kUylo  = -1.5;  // bottom of screen (over-fetch well below so the near corners stay covered)
constexpr double kUyhi  = 0.235; // the horizon (rdir.y = 0 ⇒ uy = kPitch/kFovy = 0.2333)
} // namespace ocean_grid

// The SINGLE source of the ocean geometry math: a normalized screen lattice coord (ux,uy) → raycast onto the water plane →
// FFT-displaced (ALL cascades, explicit distance-ramped LOD) → {clip position, world position}. Shared by the vertex-pull VS
// and the mesh-shader path, so they render pixel-identically. Emits the bindless cascade texture + sampler binding (set 0).
inline void ocean_projected_vertex(KGraph& g, int ux, int uy, const OceanCascadeRender& oc, int& out_clip, int& out_world)
{
    namespace kir = crd::kir;
    using namespace ocean_grid;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto add = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(kir::KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const auto mx  = [&](int a, int b) { return g.binary(kir::KOp::Max, a, b); };
    const auto mn  = [&](int a, int b) { return g.binary(kir::KOp::Min, a, b); };

    const int dx = mul(ux, k(kFovx));
    const int dy = add(k(-kPitch), mul(uy, k(kFovy)));
    // intersect the eye ray with the water plane y=0: s = kEyeH / max(−d.y, eps), clamped to the far plane. world.xz = s·d.xz.
    const int s  = mn(g.binary(kir::KOp::Div, k(kEyeH), mx(g.unary(kir::KOp::Neg, dy), k(0.02))), k(kZfar));
    const int wx = mul(s, dx);
    const int wz = s;

    const int tex  = g.texture(0, 3, kir::DType::F32, kir::TexDim::Tex2D, false, false, false, /*array_count=*/8);
    const int samp = g.sampler(0, 2);
    int       wy0  = k(0.0);
    for (int c = 0; c < oc.count; ++c)
    {
        const int    idx = g.constant(static_cast<double>(c), sh, kir::DType::U32);
        const int    uvc = g.vec2(mul(wx, k(1.0 / oc.patch[c])), mul(wz, k(1.0 / oc.patch[c])));
        const double l0  = 30.0 + 30.0 * static_cast<double>(c); // finer cascades start mipping nearer
        const double lmx = 2.0 + static_cast<double>(c);         // and ramp to a higher max LOD
        const int    lod = mn(mul(mx(sub(s, k(l0)), k(0.0)), k(lmx / 140.0)), k(lmx));
        const int    t   = g.tex_sample_at_lod(tex, samp, uvc, idx, lod);
        const int    h   = mul(sub(g.swizzle(t, 2), k(0.5)), k(2.0 * oc.hmax[c]));
        wy0              = add(wy0, mul(h, k(oc.geo_w[c])));
    }
    // taper the displacement to FLAT well BEFORE the horizon (s → far) so distant crests flatten out and the last grid rows meet
    // the sky in a clean line (no tall sawtooth silhouette). Starts at s=180, fully flat by s≈340.
    const int wy    = mul(wy0, sub(k(1.0), mn(mx(mul(sub(s, k(180.0)), k(0.00625)), k(0.0)), k(1.0))));
    // PROJECT world → clip (the exact inverse of the FS camera; no mat4). Vulkan clip: NDC.y = −uy ⇒ the sign flip.
    const int clipx = g.binary(kir::KOp::Div, wx, k(kFovx));
    const int clipy = g.unary(kir::KOp::Neg, g.binary(kir::KOp::Div, add(sub(wy, k(kEyeH)), mul(wz, k(kPitch))), k(kFovy)));
    const int clipz = mul(k(kZfar / (kZfar - kZnear)), sub(wz, k(kZnear)));
    out_clip  = g.vec4(clipx, clipy, clipz, wz);
    out_world = g.vec3(wx, wy, wz);
}

// VERTEX-PULL ocean geometry: a `grid`×`grid`-cell projected grid tessellated from `VertexIndex` (6 verts/cell, no index buffer).
// The portable path (GLSL/HLSL today). Outputs the world position; `build_ocean_water_geo_fs` shades from it.
inline void build_ocean_displaced_vs(KGraph& g, KEntry& ve, int grid, const OceanCascadeRender& oc)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto ki    = [&](int v) { return g.constant(static_cast<double>(v), sh, kir::DType::I32); };
    const auto add   = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto sub   = [&](int a, int b) { return g.binary(kir::KOp::Sub, a, b); };
    const auto mul   = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const auto idiv  = [&](int a, int b) { return g.binary(kir::KOp::Div, a, b); }; // INTEGER divide (I32 operands)

    const int vid    = g.builtin(kir::KBuiltin::VertexIndex);
    const int cell   = idiv(vid, ki(6));
    const int corner = sub(vid, mul(cell, ki(6)));                 // 0..5 (two triangles: 012, 324 → 435)
    const int cy     = idiv(cell, ki(grid));
    const int cx     = sub(cell, mul(cy, ki(grid)));
    const int e1     = g.binary(kir::KOp::CmpEq, corner, ki(1));
    const int e2     = g.binary(kir::KOp::CmpEq, corner, ki(2));
    const int e3     = g.binary(kir::KOp::CmpEq, corner, ki(3));
    const int e4     = g.binary(kir::KOp::CmpEq, corner, ki(4));
    const int e5     = g.binary(kir::KOp::CmpEq, corner, ki(5));
    const int ox     = g.select(e1, ki(1), g.select(e3, ki(1), g.select(e4, ki(1), ki(0)))); // x+1 for corners 1,3,4
    const int oy     = g.select(e2, ki(1), g.select(e4, ki(1), g.select(e5, ki(1), ki(0)))); // y+1 for corners 2,4,5
    const int fgx    = g.cast(add(cx, ox), kir::DType::F32);
    const int fgy    = g.cast(add(cy, oy), kir::DType::F32);
    const int f_g    = k(static_cast<double>(grid));

    const int ux = sub(mul(idiv(fgx, f_g), k(2.0 * ocean_grid::kUxmax)), k(ocean_grid::kUxmax));
    const int uy = add(k(ocean_grid::kUylo), mul(idiv(fgy, f_g), k(ocean_grid::kUyhi - ocean_grid::kUylo)));
    int       clip  = -1;
    int       world = -1;
    ocean_projected_vertex(g, ux, uy, oc, clip, world);

    ve.stage    = kir::KStage::Vertex;
    ve.position = clip;
    ve.n_out    = 1;
    ve.out[0]   = {world, 0, kir::Interp::Smooth}; // world position (the FS derives uv/normal/foam/depth from it)
}

// MESH-shader ocean (the modern fast path — same projected grid, emitted as MESHLETS). Each workgroup emits ONE `kk`×`kk`-vertex
// patch ((kk−1)² cells → 2(kk−1)² triangles); `np` patches per side tile the grid (WorkgroupIndex → patch; LocalInvocationIndex
// → local vertex/primitive). Reuses `ocean_projected_vertex` verbatim ⇒ pixel-identical to the VS, but as GPU-driven/cullable
// meshlets (the Nanite / RTX-Mega-Geometry substrate). Pairs with the SAME `build_ocean_water_geo_fs`. Keep max(kk², 2(kk−1)²)
// ≤ 128 (glslang's mesh workgroup cap) — e.g. kk = 8 ⇒ 98 threads.
inline void build_ocean_displaced_mesh(KGraph& g, KEntry& me, int np, int kk, const OceanCascadeRender& oc)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto ki   = [&](int v) { return g.constant(static_cast<double>(v), sh, kir::DType::I32); };
    const auto add  = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(kir::KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const auto idiv = [&](int a, int b) { return g.binary(kir::KOp::Div, a, b); };
    const auto u32  = [&](int a) { return g.cast(a, kir::DType::U32); };
    const int    cells = kk - 1;
    const int    kv    = kk * kk;
    const int    kp    = 2 * cells * cells;
    const double lat   = static_cast<double>(np * cells); // total lattice cells per side (normalization)

    const int wg  = g.cast(g.builtin(kir::KBuiltin::WorkgroupIndex), kir::DType::I32);
    const int py  = idiv(wg, ki(np));
    const int px  = sub(wg, mul(py, ki(np)));
    const int tid = g.cast(g.builtin(kir::KBuiltin::LocalInvocationIndex), kir::DType::I32);

    const int ly  = idiv(tid, ki(kk));
    const int lx  = sub(tid, mul(ly, ki(kk)));
    const int gx  = add(mul(px, ki(cells)), lx);
    const int gy  = add(mul(py, ki(cells)), ly);
    const int ux  = sub(mul(idiv(g.cast(gx, kir::DType::F32), k(lat)), k(2.0 * ocean_grid::kUxmax)), k(ocean_grid::kUxmax));
    const int uy  = add(k(ocean_grid::kUylo), mul(idiv(g.cast(gy, kir::DType::F32), k(lat)), k(ocean_grid::kUyhi - ocean_grid::kUylo)));
    int       clip  = -1;
    int       world = -1;
    ocean_projected_vertex(g, ux, uy, oc, clip, world);

    const int pcell = idiv(tid, ki(2));
    const int tri   = sub(tid, mul(pcell, ki(2)));    // 0 or 1
    const int pcy   = idiv(pcell, ki(cells));
    const int pcx   = sub(pcell, mul(pcy, ki(cells)));
    const int v00   = add(mul(pcy, ki(kk)), pcx);
    const int v10   = add(v00, ki(1));
    const int v01   = add(v00, ki(kk));
    const int v11   = add(v01, ki(1));
    const int eq0   = g.binary(kir::KOp::CmpEq, tri, ki(0));
    const int i0    = g.select(eq0, v00, v10);        // tri0 = (v00,v10,v01) · tri1 = (v10,v11,v01)
    const int i1    = g.select(eq0, v10, v11);
    const int i2    = v01;

    me.stage           = kir::KStage::Mesh;
    me.position        = clip;
    me.n_out           = 1;
    me.out[0]          = {world, 0, kir::Interp::Smooth};
    me.mesh_vertices   = static_cast<crd::u32>(kv);
    me.mesh_primitives = static_cast<crd::u32>(kp);
    me.mesh_prim       = g.vec3(u32(i0), u32(i1), u32(i2));
}

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// SHARED analytic sky — the ONE source of the ocean's atmosphere, so the SKY PASS and the water's REFLECTION agree exactly. A
// physically-motivated Rayleigh+Mie dome: deep-blue zenith → pale luminous horizon (air-mass whitening), a compact warm Mie halo
// and a tight bright sun disk; decorated with PUFFY WHITE CUMULUS (round Worley billows × a fractal break-up gives a decorative
// puff field with clear-sky gaps, sunlit billow tops + self-shadowed bases, warm where the sun grazes). `dir` = a normalized
// view / reflection ray; the sun/haze/zenith COLOURS come in as nodes (built in the caller's graph, so the time-of-day dial lives
// with the caller). `with_clouds` overlays the cumulus; `hi_detail` adds edge erosion — ON for the primary sky, OFF for the
// reflection (the choppy surface hides the detail and it keeps the reflection cheap). Returns HDR linear sky radiance.
inline int analytic_sky(KGraph& g, int dir, int ldir, int sunc, int hazeh, int zen, bool with_clouds, bool hi_detail)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace nz  = crd::kir::nodes::noise;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const auto add = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(kir::KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const auto b3  = [&](kir::KOp op, int a, int c) { return nd::detail::bin(g, op, a, c); };
    const auto sat = [&](int x) { return g.binary(kir::KOp::Max, g.binary(kir::KOp::Min, x, k(1.0)), k(0.0)); };

    const int dy   = sat(g.swizzle(dir, 1));
    const int mu   = g.binary(kir::KOp::Max, g.dot(dir, ldir), k(0.0)); // cos(view, sun)
    const int tg   = g.binary(kir::KOp::Pow, dy, k(0.42));
    const int grad = add(b3(kir::KOp::Mul, hazeh, sub(k(1.0), tg)), b3(kir::KOp::Mul, zen, tg));
    const int mie  = b3(kir::KOp::Mul, kc(0.34, 0.25, 0.15), mul(g.binary(kir::KOp::Pow, mu, k(11.0)), sub(k(1.25), tg)));
    const int disk = g.unary(kir::KOp::Exp, mul(sub(mu, k(1.0)), k(1800.0)));
    const int glow = g.unary(kir::KOp::Exp, mul(sub(mu, k(1.0)), k(60.0)));
    const int suns = b3(kir::KOp::Mul, sunc, add(mul(disk, k(18.0)), mul(glow, k(0.40))));
    const int skyc = add(add(grad, mie), suns);
    if (!with_clouds) { return skyc; }

    const int cy    = add(g.swizzle(dir, 1), k(0.42));
    const int cu    = mul(g.binary(kir::KOp::Div, g.swizzle(dir, 0), cy), k(0.85));
    const int cw    = mul(g.binary(kir::KOp::Div, g.swizzle(dir, 2), cy), k(0.85));
    const int bill  = sub(k(1.0), g.unary(kir::KOp::Sqrt, nz::worley2(g, mul(cu, k(0.95)), mul(cw, k(0.95)), 1.0, 0, 0))); // big round puffs
    const int fbm   = sat(mul(add(nz::fractal2(g, mul(cu, k(1.7)), mul(cw, k(1.7)), 4, 2.0, 0.55), k(1.0)), k(0.5)));
    const int shape = mul(fbm, add(k(0.48), mul(bill, k(0.72)))); // fbm = broad field, billows sculpt it into puffs
    int       dens  = sat(mul(sub(shape, k(0.32)), k(3.0)));
    if (hi_detail)
    {
        const int det = sat(mul(add(nz::fractal2(g, mul(cu, k(5.5)), mul(cw, k(5.5)), 3, 2.0, 0.5), k(1.0)), k(0.5)));
        dens          = sat(sub(dens, mul(det, k(0.12)))); // cauliflower edge erosion
    }
    const int hmask = sat(add(mul(g.swizzle(dir, 1), k(3.0)), k(0.14))); // decorate from just above the horizon upward
    const int cov   = mul(dens, hmask);
    const int lit   = add(k(0.70), mul(bill, k(0.55)));               // billow centres sunlit-bright
    const int shad  = sub(k(1.0), mul(dens, k(0.38)));               // thick cores self-shadow toward the base
    const int base  = b3(kir::KOp::Mul, kc(1.12, 1.13, 1.15), mul(lit, shad));
    const int warm  = b3(kir::KOp::Mul, kc(0.18, 0.11, 0.03), mul(g.binary(kir::KOp::Pow, mu, k(3.0)), dens));
    const int cloudc = add(base, warm);
    return add(b3(kir::KOp::Mul, skyc, sub(k(1.0), cov)), b3(kir::KOp::Mul, cloudc, cov)); // clouds occlude the sky
}

// The ocean surface FRAGMENT — shade the displaced geometry from the VS world position. The high-frequency CHOP lives HERE as a
// per-pixel, mip-filtered normal map (the FS has derivatives ⇒ no minification aliasing) summed with the smooth swell slope; the
// geometry carries the silhouette. Teal-green body + broad daytime sheen + turquoise sun-backlit SSS + HDR-white JOINT-Jacobian
// foam + aerial haze. Outputs alpha = 1 as a COVERAGE mask so the caller composites over the sky pass. `oc` MUST match the VS.
inline void build_ocean_water_geo_fs(KGraph& g, KEntry& fe, const OceanCascadeRender& oc)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace nz  = crd::kir::nodes::noise;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc    = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const auto add   = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto sub   = [&](int a, int b) { return g.binary(kir::KOp::Sub, a, b); };
    const auto mul   = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const auto b3    = [&](kir::KOp op, int a, int c) { return nd::detail::bin(g, op, a, c); };
    const auto sat   = [&](int x) { return g.binary(kir::KOp::Max, g.binary(kir::KOp::Min, x, k(1.0)), k(0.0)); };
    const auto sat3  = [&](int x) { return b3(kir::KOp::Max, b3(kir::KOp::Min, x, kc(1.0, 1.0, 1.0)), kc(0.0, 0.0, 0.0)); };
    const auto unit3 = [&](int vv) { return b3(kir::KOp::Div, vv, g.unary(kir::KOp::Sqrt, g.dot(vv, vv))); };

    const int world = g.stage_in(kir::KType::vec(kir::DType::F32, 3), 0, kir::Interp::Smooth);
    const int wx    = g.swizzle(world, 0);
    const int hh    = g.swizzle(world, 1);            // geometry height (swell) — for depth/SSS
    const int wz    = g.swizzle(world, 2);
    const int dist  = wz;                             // forward distance ≈ range to eye (eye at z=0)
    const int distf = sat(mul(sub(dist, k(30.0)), k(0.005)));

    const int tex  = g.texture(0, 3, kir::DType::F32, kir::TexDim::Tex2D, false, false, false, /*array_count=*/8);
    const int samp = g.sampler(0, 2);
    const int cfade = sub(k(1.0), mul(distf, k(0.35))); // fine detail eases off with distance (keeps the far field calm)
    int       slx  = k(0.0);
    int       slz  = k(0.0);
    int       fold = k(0.0);
    for (int c = 0; c < oc.count; ++c)
    {
        const int idx = g.constant(static_cast<double>(c), sh, kir::DType::U32);
        const int uvc = g.vec2(mul(wx, k(1.0 / oc.patch[c])), mul(wz, k(1.0 / oc.patch[c])));
        const int t   = g.tex_sample_at(tex, samp, uvc, idx);
        const int nxc = sub(mul(g.swizzle(t, 0), k(2.0)), k(1.0));
        const int nzc = sub(mul(g.swizzle(t, 1), k(2.0)), k(1.0));
        const int wgt = (c == 0) ? k(oc.nrm_w[c]) : mul(k(oc.nrm_w[c]), cfade); // cascade 0 (swell) never fades
        slx  = add(slx, mul(nxc, wgt));
        slz  = add(slz, mul(nzc, wgt));
        fold = add(fold, mul(mul(g.swizzle(t, 3), k(oc.foam_w[c])), cfade)); // A = ACCUMULATED temporal foam (0..1), weighted-summed
    }
    const int n = unit3(g.vec3(mul(slx, k(1.4)), k(1.0), mul(slz, k(1.4))));
    // gentle threshold → foam coverage, then TEXTURE it (bubbly, not a flat gray blob): a Worley bubble field + a fractal
    // break-up, slope-warped so it drifts with the crests; erodes the coverage into streaks/bubbles at the edges.
    const int foam_raw = sat(mul(sub(fold, k(oc.foam_bias)), k(oc.foam_scale)));
    const int fnx  = add(mul(wx, k(0.6)), mul(slx, k(0.35)));
    const int fnz  = add(mul(wz, k(0.6)), mul(slz, k(0.35)));
    const int bub  = nz::worley2(g, fnx, fnz, 1.0, 0, 0);                                   // cellular (0 at centre → 1 at edge)
    const int brk  = sat(mul(add(nz::fractal2(g, mul(fnx, k(2.4)), mul(fnz, k(2.4)), 3, 2.0, 0.5), k(1.0)), k(0.5)));
    const int ftex = sat(add(mul(sub(k(1.0), bub), k(1.15)), mul(brk, k(0.5))));            // bubble bodies + fine break-up
    const int foam = sat(mul(foam_raw, add(k(0.62), mul(ftex, k(0.55)))));                   // texture erodes the edges, keeps the cores

    const int eye   = kc(0.0, 5.0, 0.0);
    const int v     = unit3(sub(eye, world));         // surface → eye
    const int rdir  = g.unary(kir::KOp::Neg, v);      // incident view ray

    // reflection-sky constants — IDENTICAL to the sky pass (build_ocean_frame_fft_fs) so the sea MIRRORS the real sky/sun.
    const int ldir  = unit3(kc(0.30, 0.52, 0.80));
    const int sunc  = kc(2.05, 1.86, 1.55);
    const int hazeh = kc(0.56, 0.71, 0.86);
    const int zen   = kc(0.07, 0.27, 0.64);

    // Fresnel: bright-sky reflection + broad daytime sheen vs tropical-teal body + turquoise sun-backlit SSS. The reflection uses
    // the SHARED analytic sky (with clouds ⇒ the sea MIRRORS the puffy sky) — the choppy surface distorts it into realistic broken
    // reflections. hi_detail=false keeps it cheap; the erosion detail is invisible once the normal scatters the reflection anyway.
    const int nov  = g.binary(kir::KOp::Max, g.dot(n, v), k(1e-3));
    const int fr   = fresnel_water(g, nov);
    const int rfl  = sub(rdir, b3(kir::KOp::Mul, n, mul(k(2.0), g.dot(rdir, n))));
    const int skyr = analytic_sky(g, unit3(rfl), ldir, sunc, hazeh, zen, /*with_clouds=*/true, /*hi_detail=*/false);
    const int glit = ocean_sun_glitter(g, n, v, ldir, sunc, add(k(0.02), mul(distf, k(0.05))));
    const int refl = add(b3(kir::KOp::Mul, skyr, k(0.66)), b3(kir::KOp::Mul, glit, k(0.62))); // stronger sun-glitter path (ref)

    // TEAL-GREEN open-ocean body: green edges out blue up close, deepening a touch with distance; troughs darker.
    const int deep0  = add(kc(0.015, 0.085, 0.115), b3(kir::KOp::Mul, kc(0.01, 0.05, 0.075), sub(k(1.0), distf)));
    const int depthv = add(k(0.72), mul(sat(add(mul(hh, k(0.5)), k(0.5))), k(0.5)));
    const int deep   = b3(kir::KOp::Mul, deep0, depthv);
    const int vdl    = sat(g.dot(v, g.unary(kir::KOp::Neg, ldir)));
    const int sssf   = mul(sat(sub(mul(hh, k(1.3)), k(0.15))), mul(vdl, vdl));
    const int refr   = add(deep, b3(kir::KOp::Mul, kc(0.03, 0.46, 0.36), mul(sssf, k(1.15)))); // teal-green sun-scatter on lit crests

    const int foamv  = mul(foam, sub(k(1.0), mul(distf, k(0.25)))); // textured accumulated foam, eased a touch with distance
    // TEXTURED foam colour (not a flat gray/white blob): bubble cores read bright warm-white, eroded edges dimmer + a touch cooler.
    const int foamc  = add(kc(3.1, 3.3, 3.45), b3(kir::KOp::Mul, kc(1.7, 1.6, 1.35), ftex));
    const int water0 = add(b3(kir::KOp::Mul, refl, fr), b3(kir::KOp::Mul, refr, sub(k(1.0), fr)));
    const int water1 = add(b3(kir::KOp::Mul, water0, sub(k(1.0), foamv)), b3(kir::KOp::Mul, foamc, foamv));

    // aerial perspective: the DISTANCE fog tints the water body toward the hazy horizon colour; the GRAZING-horizon veil becomes
    // the output ALPHA (transparency to the REAL sky in the composite) so the far crests dissolve into the actual sky ⇒ a truly
    // SEAMLESS horizon — no dark outline where the geometry silhouette meets the sky (the earlier colour-veil left a hard edge).
    const int fogc  = hazeh; // == the sky-at-horizon colour ⇒ the far sea hazes into the SAME tone as the sky behind it
    const int fogd  = sat(sub(k(1.0), g.unary(kir::KOp::Exp, mul(dist, k(-0.0050)))));
    const int water = add(b3(kir::KOp::Mul, water1, sub(k(1.0), fogd)), b3(kir::KOp::Mul, fogc, fogd)); // distance haze on the body

    const int ce   = b3(kir::KOp::Mul, water, k(0.42)); // exposure + Narkowicz ACES tonemap + sRGB
    const int num  = b3(kir::KOp::Mul, ce, b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, k(2.51)), k(0.03)));
    const int den  = b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, k(2.43)), k(0.59))), k(0.14));
    const int aces = sat3(b3(kir::KOp::Div, num, den));
    const int srgb = b3(kir::KOp::Pow, aces, k(1.0 / 2.2));
    // HORIZON coverage: the far grid rows (the sawtooth top edge of the projected grid) DISSOLVE into the real sky over the last
    // ~55 m of range (dist∈[193,248]) ⇒ no dark mesh-edge silhouette; near/mid water stays fully opaque. The revealed sky is the
    // same hazeh tone the far water already hazed to, so the meeting line is seamless.
    const int alpha = sat(sub(k(1.0), sat(mul(sub(dist, k(193.0)), k(1.0 / 55.0)))));
    fe.stage       = kir::KStage::Fragment;
    fe.n_out       = 1;
    fe.out[0]      = {g.vec4(g.swizzle(srgb, 0), g.swizzle(srgb, 1), g.swizzle(srgb, 2), alpha), 0};
}

} // namespace crd::kir::water
