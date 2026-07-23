#pragma once

// ckir_draw.hpp — RET-6 (ADR-0105): crd-draw's shader suite as CKIR GRAPHS on the ONE graphics layer. The faithful
// ports of line_aa.{vert,frag}.glsl · triangle_solid.{vert,frag}.glsl · infinite_grid.{vert,frag}.glsl (ADR-0066 §4/5/19
// — the cooked-GLSL pack these retire). Pure crd-kir: no GPU API, no rhi, gate-testable against the CPU oracle.
//
// THE DATA CONTRACT (replaces per-instance vertex attributes + the 128-byte push-constant block): ONE u32-typed storage
// buffer at set 0 / binding 0, VERTEX+FRAGMENT visible (the GEO-1 vertex-pulling seam — no vertex-input state exists at
// all). Geometry floats travel as f32 BIT PATTERNS recovered with `int_bits_to_float` (u32 loads are bit-exact on every
// backend; packed RGBA8 colors never round-trip through a float load, so NaN canonicalization can't corrupt them).
//
//   words [0..31]  HEADER (mirrors detail::DrawPushConstants, same order):
//     [0..15] view_proj (column-major f32 bits) · [16,17] viewport_px · [18] category_mask · [19] time_s
//     [20..22] camera_pos xyz · [23] plane_y · [24] primary_color · [25] secondary_color
//     [26] primary_cell · [27] secondary_cell · [28] fade_distance · [29] axis_x_color · [30] axis_z_color · [31] reserved
//   words [32..)   INSTANCES, tightly packed:
//     line  (9 words): sx sy sz ex ey ez color_packed flags_raw width      — LineInstanceGpu's exact field order
//     tri  (11 words): v0x v0y v0z v1x v1y v1z v2x v2y v2z color_packed flags_raw
//
// Vertex counts: lines draw 6·N vertices (instance = VertexIndex/6, corner = VertexIndex%6 — the Three.js
// LineSegments2 screen-space quad), triangles 3·N, the grid a fixed 6.

#include <crd/kir/ckir.hpp>

namespace crd::draw::ckir
{

inline constexpr crd::u32 kHeaderWords       = 32U;
inline constexpr crd::u32 kLineInstanceWords = 9U;
inline constexpr crd::u32 kTriInstanceWords  = 11U;

namespace detail
{

// The shared builder scaffolding: the u32 draw buffer + f32-bit loads + the header accessors every stage uses.
// The buffer is the RASTER storage seam (`KOp::StorageLoad` — the implicit `uint data[]` SSBO at set 0 / binding 0
// that draw_storage/upload_storage bind; the GEO-1 vertex-pull idiom, see tests/gpu-shared/ckir_vertex_pull.hpp).
struct DrawGraphCtx
{
    crd::kir::KGraph& g;
    crd::kir::Shape   sh;

    explicit DrawGraphCtx(crd::kir::KGraph& graph) : g(graph), sh(crd::kir::make_shape({1})) {}

    [[nodiscard]] int kf(double v) { return g.constant(v, sh, crd::kir::DType::F32); }
    [[nodiscard]] int ku(crd::u32 v) { return g.constant(static_cast<double>(v), sh, crd::kir::DType::U32); }
    [[nodiscard]] int add(int a, int b) { return g.binary(crd::kir::KOp::Add, a, b); }
    [[nodiscard]] int sub(int a, int b) { return g.binary(crd::kir::KOp::Sub, a, b); }
    [[nodiscard]] int mul(int a, int b) { return g.binary(crd::kir::KOp::Mul, a, b); }
    [[nodiscard]] int dvd(int a, int b) { return g.binary(crd::kir::KOp::Div, a, b); }
    [[nodiscard]] int mn(int a, int b) { return g.binary(crd::kir::KOp::Min, a, b); }
    [[nodiscard]] int mx(int a, int b) { return g.binary(crd::kir::KOp::Max, a, b); }

    // load word `idx` (a u32 node) as raw u32
    [[nodiscard]] int loadu(int idx) { return g.storage_load(idx); }
    // load word `idx` reinterpreted as the f32 its bits denote (cast u32→i32 is bit-preserving two's complement)
    [[nodiscard]] int loadf(int idx) { return g.int_bits_to_float(g.cast(loadu(idx), crd::kir::DType::I32)); }
    // fixed header word accessors
    [[nodiscard]] int hdrf(crd::u32 word) { return loadf(ku(word)); }
    [[nodiscard]] int hdru(crd::u32 word) { return loadu(ku(word)); }

    // clip = view_proj · vec4(p, 1) with the column-major header matrix: clip[i] = Σ_j m[j*4+i]·v[j].
    // Returns the 4 clip components via out[4].
    void mul_view_proj(int px, int py, int pz, int out[4])
    {
        const int v[4] = {px, py, pz, kf(1.0)};
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            int acc = mul(hdrf(0U * 4U + i), v[0]);
            acc     = add(acc, mul(hdrf(1U * 4U + i), v[1]));
            acc     = add(acc, mul(hdrf(2U * 4U + i), v[2]));
            acc     = add(acc, mul(hdrf(3U * 4U + i), v[3]));
            out[i]  = acc;
        }
    }

    // unpack_rgba8: packed u32 → vec4 in [0,1] (R low byte — Color::packed_rgba()'s layout, the GLSL original's order)
    [[nodiscard]] int unpack_rgba8(int packed)
    {
        const int m255 = ku(0xFFU);
        const int inv  = kf(1.0 / 255.0);
        const auto ch  = [&](crd::u32 shift) {
            const int bits = g.binary(crd::kir::KOp::BitAnd, g.binary(crd::kir::KOp::Shr, packed, ku(shift)), m255);
            return mul(g.cast(bits, crd::kir::DType::F32), inv);
        };
        return g.vec4(ch(0U), ch(8U), ch(16U), ch(24U));
    }

    // the category-mask test of the GLSL originals: cat = (flags >> 2) & 0xF; visible iff (mask >> cat) & 1
    [[nodiscard]] int category_visible(int flags_u32)
    {
        const int cat  = g.binary(crd::kir::KOp::BitAnd, g.binary(crd::kir::KOp::Shr, flags_u32, ku(2U)), ku(0xFU));
        const int bit  = g.binary(crd::kir::KOp::BitAnd, g.binary(crd::kir::KOp::Shr, hdru(18U), cat), ku(1U));
        return g.binary(crd::kir::KOp::CmpNe, bit, ku(0U)); // Bool: this instance's category is enabled
    }
};

} // namespace detail

// ── line_aa VERTEX: the screen-space quad expansion (Three.js LineSegments2), instance = vid/6, corner = vid%6 ──────
// Interpolants: loc 0 v_color (vec4, smooth) · loc 1 v_quad_coord (vec2, smooth) · loc 2 v_width_px (f32, FLAT).
inline void build_line_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    detail::DrawGraphCtx c(g);

    const int vid    = g.builtin(kir::KBuiltin::VertexIndex);           // i32
    const int vidu   = g.cast(vid, kir::DType::U32);
    const int inst   = c.dvd(vidu, c.ku(6U));                            // instance index (u32 div truncates)
    const int corner = c.sub(vidu, c.mul(inst, c.ku(6U)));               // vid % 6
    const int base   = c.add(c.ku(kHeaderWords), c.mul(inst, c.ku(kLineInstanceWords)));

    // per-instance fields (LineInstanceGpu order)
    const int sx = c.loadf(c.add(base, c.ku(0U)));
    const int sy = c.loadf(c.add(base, c.ku(1U)));
    const int sz = c.loadf(c.add(base, c.ku(2U)));
    const int ex = c.loadf(c.add(base, c.ku(3U)));
    const int ey = c.loadf(c.add(base, c.ku(4U)));
    const int ez = c.loadf(c.add(base, c.ku(5U)));
    const int col = c.loadu(c.add(base, c.ku(6U)));
    const int flg = c.loadu(c.add(base, c.ku(7U)));
    const int wid = c.loadf(c.add(base, c.ku(8U)));

    // the 6-corner table via a select chain (SROA-safe): x = {0,1,0,0,1,1}[corner], y = {-1,-1,1,1,-1,1}[corner]
    const auto ceq = [&](crd::u32 k) { return g.binary(kir::KOp::CmpEq, corner, c.ku(k)); };
    const int  cx  = g.select(ceq(0U), c.kf(0.0),
                     g.select(ceq(2U), c.kf(0.0), g.select(ceq(3U), c.kf(0.0), c.kf(1.0)))); // 1,4,5 → 1.0
    const int  cy  = g.select(ceq(0U), c.kf(-1.0),
                     g.select(ceq(1U), c.kf(-1.0), g.select(ceq(4U), c.kf(-1.0), c.kf(1.0)))); // 2,3,5 → +1

    // clip-space endpoints
    int ca[4];
    int cb[4];
    c.mul_view_proj(sx, sy, sz, ca);
    c.mul_view_proj(ex, ey, ez, cb);

    // NDC → pixel positions (the GLSL original's math, term for term)
    const int vpx  = c.hdrf(16U);
    const int vpy  = c.hdrf(17U);
    const int ax   = c.mul(c.dvd(ca[0], ca[3]), c.mul(c.kf(0.5), vpx));
    const int ay   = c.mul(c.dvd(ca[1], ca[3]), c.mul(c.kf(0.5), vpy));
    const int bx   = c.mul(c.dvd(cb[0], cb[3]), c.mul(c.kf(0.5), vpx));
    const int by   = c.mul(c.dvd(cb[1], cb[3]), c.mul(c.kf(0.5), vpy));

    // screen-space direction; degenerate guard len < 1e-3 → dir = (1,0), len = 1
    const int dx0  = c.sub(bx, ax);
    const int dy0  = c.sub(by, ay);
    const int len0 = g.unary(kir::KOp::Sqrt, c.add(c.mul(dx0, dx0), c.mul(dy0, dy0)));
    const int dgn  = g.binary(kir::KOp::CmpLt, len0, c.kf(1.0e-3));
    const int len  = g.select(dgn, c.kf(1.0), len0);
    const int dirx = g.select(dgn, c.kf(1.0), c.dvd(dx0, len));
    const int diry = g.select(dgn, c.kf(0.0), c.dvd(dy0, len));

    // perpendicular push by half-width, lerp along the line by corner.x
    const int halfw = c.mul(wid, c.kf(0.5));
    const int basex = c.add(ax, c.mul(c.sub(bx, ax), cx)); // mix(px_a, px_b, corner.x)
    const int basey = c.add(ay, c.mul(c.sub(by, ay), cx));
    const int fx    = c.add(basex, c.mul(g.unary(kir::KOp::Neg, diry), c.mul(cy, halfw)));
    const int fy    = c.add(basey, c.mul(dirx, c.mul(cy, halfw)));

    // back to NDC, with the matching per-end w and z (perspective-correct)
    const int ndcx = c.mul(fx, c.dvd(c.kf(2.0), vpx));
    const int ndcy = c.mul(fy, c.dvd(c.kf(2.0), vpy));
    const int w    = c.add(ca[3], c.mul(c.sub(cb[3], ca[3]), cx));
    const int za   = c.dvd(ca[2], ca[3]);
    const int zb   = c.dvd(cb[2], cb[3]);
    const int z    = c.add(za, c.mul(c.sub(zb, za), cx));

    // category mask: collapse to (2,2,2,1) outside NDC when the category bit is off
    const int vis  = c.category_visible(flg);
    const int px   = g.select(vis, c.mul(ndcx, w), c.kf(2.0));
    const int py   = g.select(vis, c.mul(ndcy, w), c.kf(2.0));
    const int pz   = g.select(vis, c.mul(z, w), c.kf(2.0));
    const int pw   = g.select(vis, w, c.kf(1.0));

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(px, py, pz, pw);
    ve.n_out    = 3;
    ve.out[0]   = {c.unpack_rgba8(col), 0, kir::Interp::Smooth};       // v_color
    ve.out[1]   = {g.vec2(cx, cy), 1, kir::Interp::Smooth};            // v_quad_coord
    ve.out[2]   = {wid, 2, kir::Interp::Flat};                          // v_width_px
}

// ── line_aa FRAGMENT: 1-pixel edge falloff from |quad.y| — alpha = 1 − smoothstep(1−fade, 1, d) ────────────────────
inline void build_line_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    detail::DrawGraphCtx c(g);

    const int vcol  = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 0, kir::Interp::Smooth);
    const int vquad = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 1, kir::Interp::Smooth);
    const int vwid  = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 2, kir::Interp::Flat);

    const int d      = g.unary(kir::KOp::Abs, g.swizzle(vquad, 1));
    const int half_w = c.mx(c.mul(vwid, c.kf(0.5)), c.kf(0.5));
    const int fade   = c.dvd(c.kf(1.0), half_w);
    const int edge0  = c.sub(c.kf(1.0), fade);
    const int alpha  = c.sub(c.kf(1.0), g.ternary(kir::KOp::Smoothstep, edge0, c.kf(1.0), d));

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(vcol, 0), g.swizzle(vcol, 1), g.swizzle(vcol, 2),
                        c.mul(g.swizzle(vcol, 3), alpha)),
                 0};
}

// ── triangle_solid VERTEX: instance = vid/3, corner select over 3 world-space corners ──────────────────────────────
inline void build_tri_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    detail::DrawGraphCtx c(g);

    const int vidu   = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::U32);
    const int inst   = c.dvd(vidu, c.ku(3U));
    const int corner = c.sub(vidu, c.mul(inst, c.ku(3U)));
    const int base   = c.add(c.ku(kHeaderWords), c.mul(inst, c.ku(kTriInstanceWords)));

    // corner-select the world position: word offset = corner*3 + axis (dynamic index via 2-select chain per axis)
    const auto axis = [&](crd::u32 a) {
        const int w0 = c.loadf(c.add(base, c.ku(a)));
        const int w1 = c.loadf(c.add(base, c.ku(3U + a)));
        const int w2 = c.loadf(c.add(base, c.ku(6U + a)));
        const int e0 = g.binary(kir::KOp::CmpEq, corner, c.ku(0U));
        const int e1 = g.binary(kir::KOp::CmpEq, corner, c.ku(1U));
        return g.select(e0, w0, g.select(e1, w1, w2));
    };
    const int wx  = axis(0U);
    const int wy  = axis(1U);
    const int wz  = axis(2U);
    const int col = c.loadu(c.add(base, c.ku(9U)));
    const int flg = c.loadu(c.add(base, c.ku(10U)));

    int clip[4];
    c.mul_view_proj(wx, wy, wz, clip);

    const int vis = c.category_visible(flg);
    ve.stage      = kir::KStage::Vertex;
    ve.position   = g.vec4(g.select(vis, clip[0], c.kf(2.0)), g.select(vis, clip[1], c.kf(2.0)),
                           g.select(vis, clip[2], c.kf(2.0)), g.select(vis, clip[3], c.kf(1.0)));
    ve.n_out      = 1;
    ve.out[0]     = {c.unpack_rgba8(col), 0, kir::Interp::Smooth};
}

// ── triangle_solid FRAGMENT: pass-through color (alpha blending composes it over the scene) ────────────────────────
inline void build_tri_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const int vcol = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 0, kir::Interp::Smooth);
    fe.stage       = kir::KStage::Fragment;
    fe.n_out       = 1;
    fe.out[0]      = {vcol, 0};
}

// ── infinite_grid VERTEX: a camera-anchored XZ quad at plane_y, half-extent = max(fade_distance, 1) ────────────────
inline void build_grid_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    detail::DrawGraphCtx c(g);

    const int vidu   = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::U32);
    const int corner = c.sub(vidu, c.mul(c.dvd(vidu, c.ku(6U)), c.ku(6U)));
    // corners: x = {-1,1,1,-1,1,-1}[c], y = {-1,-1,1,-1,1,1}[c]
    const auto ceq = [&](crd::u32 k) { return g.binary(kir::KOp::CmpEq, corner, c.ku(k)); };
    const int  cx  = g.select(ceq(0U), c.kf(-1.0), g.select(ceq(3U), c.kf(-1.0), g.select(ceq(5U), c.kf(-1.0), c.kf(1.0))));
    const int  cz  = g.select(ceq(0U), c.kf(-1.0), g.select(ceq(1U), c.kf(-1.0), g.select(ceq(3U), c.kf(-1.0), c.kf(1.0))));

    const int half = c.mx(c.hdrf(28U), c.kf(1.0));
    const int wx   = c.add(c.hdrf(20U), c.mul(cx, half));
    const int wy   = c.hdrf(23U);
    const int wz   = c.add(c.hdrf(22U), c.mul(cz, half));

    int clip[4];
    c.mul_view_proj(wx, wy, wz, clip);

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(clip[0], clip[1], clip[2], clip[3]);
    ve.n_out    = 1;
    ve.out[0]   = {g.vec3(wx, wy, wz), 0, kir::Interp::Smooth}; // v_world_pos
}

// ── infinite_grid FRAGMENT: the pristine-grid cell factor (fract/fwidth), quadratic camera fade, axis highlights ───
inline void build_grid_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    detail::DrawGraphCtx c(g);

    const int wpos = g.stage_in(kir::KType::vec(kir::DType::F32, 3), 0, kir::Interp::Smooth);
    const int wx   = g.swizzle(wpos, 0);
    const int wz   = g.swizzle(wpos, 2);

    const auto clamp01 = [&](int x) { return c.mn(c.mx(x, c.kf(0.0)), c.kf(1.0)); };
    // grid_factor(coord, cell): g = |fract(coord/cell − .5) − .5| / fwidth(coord/cell); 1 − clamp(min(g.x,g.y))
    const auto grid_factor = [&](crd::u32 cell_word) {
        const int cell = c.hdrf(cell_word);
        const int ux   = c.dvd(wx, cell);
        const int uz   = c.dvd(wz, cell);
        const auto lf  = [&](int u) {
            const int fr = g.unary(kir::KOp::Fract, c.sub(u, c.kf(0.5)));
            const int av = g.unary(kir::KOp::Abs, c.sub(fr, c.kf(0.5)));
            return c.dvd(av, g.unary(kir::KOp::Fwidth, u));
        };
        return c.sub(c.kf(1.0), clamp01(c.mn(lf(ux), lf(uz))));
    };
    const int primary   = grid_factor(26U);
    const int secondary = grid_factor(27U);

    // quadratic distance fade in XZ from the camera
    const int dxc  = c.sub(wx, c.hdrf(20U));
    const int dzc  = c.sub(wz, c.hdrf(22U));
    const int dist = g.unary(kir::KOp::Sqrt, c.add(c.mul(dxc, dxc), c.mul(dzc, dzc)));
    const int f0   = c.sub(c.kf(1.0), clamp01(c.dvd(dist, c.mx(c.hdrf(28U), c.kf(1.0)))));
    const int fade = c.mul(f0, f0);

    const int col_p = c.unpack_rgba8(c.hdru(24U));
    const int col_s = c.unpack_rgba8(c.hdru(25U));

    // compose per component: result = mix(secondary·col_s, col_p, primary), then the axis highlights on top
    const int ax_f = c.sub(c.kf(1.0), clamp01(c.dvd(g.unary(kir::KOp::Abs, wx), g.unary(kir::KOp::Fwidth, wx))));
    const int az_f = c.sub(c.kf(1.0), clamp01(c.dvd(g.unary(kir::KOp::Abs, wz), g.unary(kir::KOp::Fwidth, wz))));
    const int col_ax = c.unpack_rgba8(c.hdru(29U)); // X axis color (the line at z=0)
    const int col_az = c.unpack_rgba8(c.hdru(30U)); // Z axis color (the line at x=0)

    int out_c[4];
    for (crd::u32 k = 0; k < 4U; ++k)
    {
        const int sec = c.mul(g.swizzle(col_s, static_cast<int>(k)), secondary);
        const int pri = g.swizzle(col_p, static_cast<int>(k));
        int v         = c.add(sec, c.mul(c.sub(pri, sec), primary));       // mix(sec·s, pri, primary)
        v             = c.add(v, c.mul(c.sub(g.swizzle(col_ax, static_cast<int>(k)), v), az_f)); // X axis at z=0
        v             = c.add(v, c.mul(c.sub(g.swizzle(col_az, static_cast<int>(k)), v), ax_f)); // Z axis at x=0
        out_c[k]      = v;
    }

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(out_c[0], out_c[1], out_c[2], c.mul(out_c[3], fade)), 0};
}

} // namespace crd::draw::ckir
