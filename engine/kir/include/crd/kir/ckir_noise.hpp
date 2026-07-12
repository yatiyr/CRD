#pragma once

// ckir_noise.hpp — the MaterialX SOURCE noise nodes (D-007 B6-b), a faithful CKIR transcription of MaterialX's
// libraries/stdlib/genglsl/lib/mx_noise.glsl (itself the OSL oslnoise subset — "results identical to the OSL noise
// functions"). Perlin (improved-gradient), cell, worley, and fractal(fBm) — each bit-exact vs that reference.
//
// ── THE 32-BIT HASH ON A f64/i64 IR (the crux) ───────────────────────────────────────────────────────────────────────
// The Bob-Jenkins hash (mx_bjmix/mx_bjfinal/mx_rotl32) is 32-bit UNSIGNED integer arithmetic with wraparound. CKIR's
// bitwise ops reinterpret an f64 node through i64, so two facts must be engineered for bit-exactness:
//   1. ROTATE without exceeding f64's 53-bit exact range. `x<<25` for a 32-bit x overflows 2^53 and would round. So
//      rotl32 is written in the algebraically-equal SPLIT form  ((x & (0xFFFFFFFF>>k)) << k) | (x >> (32-k))  — every
//      intermediate stays < 2^32, hence exact.
//   2. WRAPAROUND add/sub. Every `a += b` / `a -= b` is followed by `& 0xFFFFFFFF`: on the CPU oracle this yields the
//      mod-2^32 value (two's-complement i64 AND 0xFFFFFFFF == the uint32), and on the GPU the nodes are U32-typed so
//      `uint` already wraps and the mask is a no-op — both agree.
// The hash nodes are U32-typed so the GLSL/HLSL emitters render `uint` (making `>>` a LOGICAL shift — essential, since a
// full 32-bit hash has bit 31 set). Coordinates are floored to an integer, cast to U32 (well-defined int→uint), and fed in.

#include <crd/kir/ckir.hpp>

namespace crd::kir::nodes::noise
{

namespace detail
{
// a U32 constant shaped like node `like` (so a same-shape bitwise op is legal); its VALUE carries the integer.
[[nodiscard]] inline int ku(KGraph& g, int like, crd::u32 v) { return g.constant(static_cast<crd::f64>(v), g.node(like).shape, DType::U32); }
// a float constant shaped+dtyped like node `like` (F32 on the GPU path, F64 in the exact CPU-oracle test).
[[nodiscard]] inline int kf(KGraph& g, int like, crd::f64 v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
[[nodiscard]] inline int band(KGraph& g, int a, crd::u32 m) { return g.binary(KOp::BitAnd, a, ku(g, a, m)); }
[[nodiscard]] inline int bxor(KGraph& g, int a, int b) { return g.binary(KOp::BitXor, a, b); }
[[nodiscard]] inline int bor(KGraph& g, int a, int b) { return g.binary(KOp::BitOr, a, b); }
[[nodiscard]] inline int shl(KGraph& g, int a, crd::u32 k) { return g.binary(KOp::Shl, a, ku(g, a, k)); }
[[nodiscard]] inline int shr(KGraph& g, int a, crd::u32 k) { return g.binary(KOp::Shr, a, ku(g, a, k)); }
// 32-bit wraparound add / sub: mask to 32 bits so the CPU oracle matches the GPU's native uint wrap.
[[nodiscard]] inline int add32(KGraph& g, int a, int b) { return band(g, g.binary(KOp::Add, a, b), 0xFFFFFFFFU); }
[[nodiscard]] inline int sub32(KGraph& g, int a, int b) { return band(g, g.binary(KOp::Sub, a, b), 0xFFFFFFFFU); }
// mx_rotl32 in the split (overflow-free) form.
[[nodiscard]] inline int rotl32(KGraph& g, int x, crd::u32 k) { return bor(g, shl(g, band(g, x, 0xFFFFFFFFU >> k), k), shr(g, x, 32U - k)); }

// mx_bjmix(inout a,b,c): thread the three node ids through the six rounds.
inline void bjmix(KGraph& g, int& a, int& b, int& c)
{
    a = sub32(g, a, c); a = bxor(g, a, rotl32(g, c, 4U));  c = add32(g, c, b);
    b = sub32(g, b, a); b = bxor(g, b, rotl32(g, a, 6U));  a = add32(g, a, c);
    c = sub32(g, c, b); c = bxor(g, c, rotl32(g, b, 8U));  b = add32(g, b, a);
    a = sub32(g, a, c); a = bxor(g, a, rotl32(g, c, 16U)); c = add32(g, c, b);
    b = sub32(g, b, a); b = bxor(g, b, rotl32(g, a, 19U)); a = add32(g, a, c);
    c = sub32(g, c, b); c = bxor(g, c, rotl32(g, b, 4U));  b = add32(g, b, a);
}
// mx_bjfinal(a,b,c) -> hash.
[[nodiscard]] inline int bjfinal(KGraph& g, int a, int b, int c)
{
    c = bxor(g, c, b); c = sub32(g, c, rotl32(g, b, 14U));
    a = bxor(g, a, c); a = sub32(g, a, rotl32(g, c, 11U));
    b = bxor(g, b, a); b = sub32(g, b, rotl32(g, a, 25U));
    c = bxor(g, c, b); c = sub32(g, c, rotl32(g, b, 16U));
    a = bxor(g, a, c); a = sub32(g, a, rotl32(g, c, 4U));
    b = bxor(g, b, a); b = sub32(g, b, rotl32(g, a, 14U));
    c = bxor(g, c, b); c = sub32(g, c, rotl32(g, b, 24U));
    return c;
}
inline constexpr crd::u32 kSeedBase = 0xdeadbeefU;
[[nodiscard]] inline int seed_u(KGraph& g, int like, crd::u32 len) { return ku(g, like, kSeedBase + (len << 2U) + 13U); }

// integer coordinate -> uint (int->uint reinterpret); the coord arrives as a float-holding-integer, cast to U32.
[[nodiscard]] inline int to_u(KGraph& g, int xi) { return g.cast(xi, DType::U32); }

// mx_hash_int(x,y): a=b=c=seed; a+=x; b+=y; return bjfinal(a,b,c).
[[nodiscard]] inline int hash2(KGraph& g, int xi, int yi)
{
    const int seed = seed_u(g, xi, 2U);
    const int a    = add32(g, seed, to_u(g, xi));
    const int b    = add32(g, seed, to_u(g, yi));
    return bjfinal(g, a, b, seed);
}
// mx_hash_int(x,y,z): a=b=c=seed; a+=x; b+=y; c+=z; return bjfinal(a,b,c).
[[nodiscard]] inline int hash3(KGraph& g, int xi, int yi, int zi)
{
    const int seed = seed_u(g, xi, 3U);
    const int a    = add32(g, seed, to_u(g, xi));
    const int b    = add32(g, seed, to_u(g, yi));
    const int c    = add32(g, seed, to_u(g, zi));
    return bjfinal(g, a, b, c);
}

// mx_floorfrac: returns the fractional part; writes the integer floor (as an I32-typed node) to `out_i`.
[[nodiscard]] inline int floorfrac(KGraph& g, int p, int& out_i)
{
    const int fl = g.unary(KOp::Floor, p);
    out_i        = g.cast(fl, DType::I32); // int(floor(p))
    return g.binary(KOp::Sub, p, fl);      // p - floor(p)
}
// mx_fade(t) = t*t*t*(t*(t*6-15)+10).
[[nodiscard]] inline int fade(KGraph& g, int t)
{
    const auto k   = [&](double v) { return kf(g, t, v); };
    const int  in  = g.binary(KOp::Add, g.binary(KOp::Mul, t, g.binary(KOp::Sub, g.binary(KOp::Mul, t, k(6.0)), k(15.0))), k(10.0)); // t*(t*6-15)+10
    return g.binary(KOp::Mul, g.binary(KOp::Mul, g.binary(KOp::Mul, t, t), t), in);                                                  // t*t*t*(...)
}
// mx_bilerp(v0,v1,v2,v3,s,t) = (1-t)*(v0*s1+v1*s) + t*(v2*s1+v3*s), s1=1-s.
[[nodiscard]] inline int bilerp(KGraph& g, int v0, int v1, int v2, int v3, int s, int t)
{
    const int one = kf(g, s, 1.0);
    const int s1  = g.binary(KOp::Sub, one, s);
    const int t1  = g.binary(KOp::Sub, one, t);
    const int lo  = g.binary(KOp::Add, g.binary(KOp::Mul, v0, s1), g.binary(KOp::Mul, v1, s));
    const int hi  = g.binary(KOp::Add, g.binary(KOp::Mul, v2, s1), g.binary(KOp::Mul, v3, s));
    return g.binary(KOp::Add, g.binary(KOp::Mul, t1, lo), g.binary(KOp::Mul, t, hi));
}
// mx_trilerp — 8-corner interpolation.
[[nodiscard]] inline int trilerp(KGraph& g, const int v[8], int s, int t, int r)
{
    const int one = kf(g, s, 1.0);
    const int s1  = g.binary(KOp::Sub, one, s);
    const int t1  = g.binary(KOp::Sub, one, t);
    const int r1  = g.binary(KOp::Sub, one, r);
    const auto lp = [&](int a, int b) { return g.binary(KOp::Add, g.binary(KOp::Mul, a, s1), g.binary(KOp::Mul, b, s)); };
    const int  a0 = g.binary(KOp::Add, g.binary(KOp::Mul, t1, lp(v[0], v[1])), g.binary(KOp::Mul, t, lp(v[2], v[3])));
    const int  a1 = g.binary(KOp::Add, g.binary(KOp::Mul, t1, lp(v[4], v[5])), g.binary(KOp::Mul, t, lp(v[6], v[7])));
    return g.binary(KOp::Add, g.binary(KOp::Mul, r1, a0), g.binary(KOp::Mul, r, a1));
}
// mx_negate_if(val, cond) = cond ? -val : val.
[[nodiscard]] inline int negate_if(KGraph& g, int val, int cond_nonzero) { return g.select(cond_nonzero, g.unary(KOp::Neg, val), val); }
// mx_gradient_float(hash, x, y): 8-direction 2D gradient dot.
[[nodiscard]] inline int gradient2(KGraph& g, int hash, int x, int y)
{
    const int h    = band(g, hash, 7U);
    const int hlt4 = g.binary(KOp::CmpLt, h, ku(g, hash, 4U));
    const int u    = g.select(hlt4, x, y);
    const int v    = g.binary(KOp::Mul, kf(g, x, 2.0), g.select(hlt4, y, x));
    const int nu   = negate_if(g, u, g.binary(KOp::CmpNe, band(g, h, 1U), ku(g, hash, 0U)));
    const int nv   = negate_if(g, v, g.binary(KOp::CmpNe, band(g, h, 2U), ku(g, hash, 0U)));
    return g.binary(KOp::Add, nu, nv);
}
// mx_gradient_float(hash, x, y, z): 12+-direction 3D gradient dot (cube-edge vectors).
[[nodiscard]] inline int gradient3(KGraph& g, int hash, int x, int y, int z)
{
    const int h     = band(g, hash, 15U);
    const int hlt8  = g.binary(KOp::CmpLt, h, ku(g, hash, 8U));
    const int hlt4  = g.binary(KOp::CmpLt, h, ku(g, hash, 4U));
    const int h12or14 = g.binary(KOp::BitOr, g.binary(KOp::CmpEq, h, ku(g, hash, 12U)), g.binary(KOp::CmpEq, h, ku(g, hash, 14U)));
    const int u     = g.select(hlt8, x, y);
    const int v     = g.select(hlt4, y, g.select(h12or14, x, z));
    const int nu    = negate_if(g, u, g.binary(KOp::CmpNe, band(g, h, 1U), ku(g, hash, 0U)));
    const int nv    = negate_if(g, v, g.binary(KOp::CmpNe, band(g, h, 2U), ku(g, hash, 0U)));
    return g.binary(KOp::Add, nu, nv);
}
} // namespace detail

// ── PERLIN noise (improved gradient), scalar output. mx_perlin_noise_float. ──────────────────────────────────────────
// perlin_2d(p): p a vec2 (or two scalars). Returns a scalar in ~[-1,1]. Scale 0.6616 (mx_gradient_scale2d).
[[nodiscard]] inline int perlin2(KGraph& g, int px, int py)
{
    namespace d = detail;
    int       xi = 0;
    int       yi = 0;
    const int fx = d::floorfrac(g, px, xi);
    const int fy = d::floorfrac(g, py, yi);
    const int u  = d::fade(g, fx);
    const int v  = d::fade(g, fy);
    const int x1 = g.binary(KOp::Add, xi, g.constant(1.0, g.node(xi).shape, DType::I32));
    const int y1 = g.binary(KOp::Add, yi, g.constant(1.0, g.node(yi).shape, DType::I32));
    const int fx1 = g.binary(KOp::Sub, fx, d::kf(g, fx, 1.0));
    const int fy1 = g.binary(KOp::Sub, fy, d::kf(g, fy, 1.0));
    const int g00 = d::gradient2(g, d::hash2(g, xi, yi), fx, fy);
    const int g10 = d::gradient2(g, d::hash2(g, x1, yi), fx1, fy);
    const int g01 = d::gradient2(g, d::hash2(g, xi, y1), fx, fy1);
    const int g11 = d::gradient2(g, d::hash2(g, x1, y1), fx1, fy1);
    return g.binary(KOp::Mul, d::kf(g, px, 0.6616), d::bilerp(g, g00, g10, g01, g11, u, v));
}
// perlin_3d(p): three scalars. Scale 0.9820 (mx_gradient_scale3d).
[[nodiscard]] inline int perlin3(KGraph& g, int px, int py, int pz)
{
    namespace d = detail;
    int       xi = 0;
    int       yi = 0;
    int       zi = 0;
    const int fx = d::floorfrac(g, px, xi);
    const int fy = d::floorfrac(g, py, yi);
    const int fz = d::floorfrac(g, pz, zi);
    const int u  = d::fade(g, fx);
    const int v  = d::fade(g, fy);
    const int w  = d::fade(g, fz);
    const auto i1 = [&](int a) { return g.binary(KOp::Add, a, g.constant(1.0, g.node(a).shape, DType::I32)); };
    const int  x1 = i1(xi);
    const int  y1 = i1(yi);
    const int  z1 = i1(zi);
    const auto f1 = [&](int a) { return g.binary(KOp::Sub, a, d::kf(g, a, 1.0)); };
    const int  fx1 = f1(fx);
    const int  fy1 = f1(fy);
    const int  fz1 = f1(fz);
    const int corners[8] = {
        d::gradient3(g, d::hash3(g, xi, yi, zi), fx, fy, fz),
        d::gradient3(g, d::hash3(g, x1, yi, zi), fx1, fy, fz),
        d::gradient3(g, d::hash3(g, xi, y1, zi), fx, fy1, fz),
        d::gradient3(g, d::hash3(g, x1, y1, zi), fx1, fy1, fz),
        d::gradient3(g, d::hash3(g, xi, yi, z1), fx, fy, fz1),
        d::gradient3(g, d::hash3(g, x1, yi, z1), fx1, fy, fz1),
        d::gradient3(g, d::hash3(g, xi, y1, z1), fx, fy1, fz1),
        d::gradient3(g, d::hash3(g, x1, y1, z1), fx1, fy1, fz1),
    };
    return g.binary(KOp::Mul, d::kf(g, px, 0.9820), d::trilerp(g, corners, u, v, w));
}

// ── CELL noise: mx_cell_noise_float — hash of the floored integer lattice cell, mapped to [0,1] via bits/0xffffffff. ──
[[nodiscard]] inline int cell2(KGraph& g, int px, int py)
{
    namespace d  = detail;
    const int xi = g.cast(g.unary(KOp::Floor, px), DType::I32);
    const int yi = g.cast(g.unary(KOp::Floor, py), DType::I32);
    const int h  = d::hash2(g, xi, yi);
    return g.binary(KOp::Div, g.cast(h, g.node(px).dtype()), d::kf(g, px, static_cast<crd::f64>(0xffffffffU))); // bits/0xffffffff
}
[[nodiscard]] inline int cell3(KGraph& g, int px, int py, int pz)
{
    namespace d  = detail;
    const int xi = g.cast(g.unary(KOp::Floor, px), DType::I32);
    const int yi = g.cast(g.unary(KOp::Floor, py), DType::I32);
    const int zi = g.cast(g.unary(KOp::Floor, pz), DType::I32);
    const int h  = d::hash3(g, xi, yi, zi);
    return g.binary(KOp::Div, g.cast(h, g.node(px).dtype()), d::kf(g, px, static_cast<crd::f64>(0xffffffffU)));
}

// ── FRACTAL (fBm) over perlin: mx_fractal2d/3d_noise_float — octave sum with lacunarity + diminish. ──────────────────
// result = sum_{o<octaves} amplitude * perlin(p * freq); freq *= lacunarity, amplitude *= diminish. Octaves is a COMPILE-
// TIME count (unrolled), matching the shader's `for` with a constant bound.
[[nodiscard]] inline int fractal2(KGraph& g, int px, int py, int octaves, double lacunarity, double diminish)
{
    namespace d      = detail;
    int          acc = d::kf(g, px, 0.0);
    int          fx  = px;
    int          fy  = py;
    double       amp = 1.0;
    for (int o = 0; o < octaves; ++o)
    {
        acc = g.binary(KOp::Add, acc, g.binary(KOp::Mul, d::kf(g, px, amp), perlin2(g, fx, fy)));
        fx  = g.binary(KOp::Mul, fx, d::kf(g, fx, lacunarity));
        fy  = g.binary(KOp::Mul, fy, d::kf(g, fy, lacunarity));
        amp *= diminish;
    }
    return acc;
}
[[nodiscard]] inline int fractal3(KGraph& g, int px, int py, int pz, int octaves, double lacunarity, double diminish)
{
    namespace d      = detail;
    int          acc = d::kf(g, px, 0.0);
    int          fx  = px;
    int          fy  = py;
    int          fz  = pz;
    double       amp = 1.0;
    for (int o = 0; o < octaves; ++o)
    {
        acc = g.binary(KOp::Add, acc, g.binary(KOp::Mul, d::kf(g, px, amp), perlin3(g, fx, fy, fz)));
        fx  = g.binary(KOp::Mul, fx, d::kf(g, fx, lacunarity));
        fy  = g.binary(KOp::Mul, fy, d::kf(g, fy, lacunarity));
        fz  = g.binary(KOp::Mul, fz, d::kf(g, fz, lacunarity));
        amp *= diminish;
    }
    return acc;
}

// ── WORLEY (cellular) noise: mx_worley_noise_float — nearest jittered-cell distance over a 3×3(×3) neighbourhood. ───────
// jitter/style/metric are COMPILE-TIME (the shader `for` has constant bounds; the metric/style `if`s fold). metric: 0=
// Euclidean · 1=distance² · 2=Manhattan · 3=Chebyshev. style: 0=distance · 1=per-cell cell-noise value.
namespace detail
{
inline constexpr crd::u32 kU32Max = 0xffffffffU;
// mx_cell_noise_vec3(vec2).xy — two hash channels of the integer cell, each mapped to [0,1].
[[nodiscard]] inline int cell01_2(KGraph& g, int ax, int ay, crd::u32 chan, int like)
{
    const int zc = g.constant(static_cast<crd::f64>(chan), g.node(ax).shape, DType::I32);
    return g.binary(KOp::Div, g.cast(hash3(g, ax, ay, zc), g.node(like).dtype()), kf(g, like, static_cast<crd::f64>(kU32Max)));
}
// mx_worley_cell_position offset (2D): ((cellnoise.xy - 0.5) * jitter + 0.5).
[[nodiscard]] inline int worley_off2(KGraph& g, int ax, int ay, double jitter, int like)
{
    const int off  = g.vec2(cell01_2(g, ax, ay, 0U, like), cell01_2(g, ax, ay, 1U, like));
    const int half = g.splat(kf(g, like, 0.5), 2);
    const int jit  = g.splat(kf(g, like, jitter), 2);
    return g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Sub, off, half), jit), half);
}
// the metric distance from a diff vector (compile-time metric).
[[nodiscard]] inline int worley_metric(KGraph& g, int diff, int metric, int ncomp_)
{
    if (metric == 2) // Manhattan: sum |comp|
    {
        int acc = g.unary(KOp::Abs, g.swizzle(diff, 0));
        for (int k = 1; k < ncomp_; ++k) { acc = g.binary(KOp::Add, acc, g.unary(KOp::Abs, g.swizzle(diff, k))); }
        return acc;
    }
    if (metric == 3) // Chebyshev: max |comp|
    {
        int acc = g.unary(KOp::Abs, g.swizzle(diff, 0));
        for (int k = 1; k < ncomp_; ++k) { acc = g.binary(KOp::Max, acc, g.unary(KOp::Abs, g.swizzle(diff, k))); }
        return acc;
    }
    return g.dot(diff, diff); // Euclidean(0) / distance²(1) — sqrt applied by the caller for metric 0
}
} // namespace detail

[[nodiscard]] inline int worley2(KGraph& g, int px, int py, double jitter, int style, int metric)
{
    namespace d = detail;
    int        xi = 0;
    int        yi = 0;
    const int  localpos = g.vec2(d::floorfrac(g, px, xi), d::floorfrac(g, py, yi));
    int        best     = d::kf(g, px, 1e6);
    int        minpos   = g.vec2(d::kf(g, px, 0.0), d::kf(g, px, 0.0));
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            const int ax   = g.binary(KOp::Add, xi, g.constant(static_cast<double>(x), g.node(xi).shape, DType::I32));
            const int ay   = g.binary(KOp::Add, yi, g.constant(static_cast<double>(y), g.node(yi).shape, DType::I32));
            const int cell = g.binary(KOp::Add, g.vec2(d::kf(g, px, static_cast<double>(x)), d::kf(g, px, static_cast<double>(y))), d::worley_off2(g, ax, ay, jitter, px));
            const int diff = g.binary(KOp::Sub, cell, localpos);
            const int dist = d::worley_metric(g, diff, metric, 2);
            const int cond = g.binary(KOp::CmpLt, dist, best);
            best           = g.select(cond, dist, best);
            if (style == 1) { minpos = g.select(cond, diff, minpos); }
        }
    }
    if (style == 1)
    {
        const int mp = g.binary(KOp::Add, minpos, g.vec2(px, py));
        return cell2(g, g.swizzle(mp, 0), g.swizzle(mp, 1));
    }
    return metric == 0 ? g.unary(KOp::Sqrt, best) : best;
}

namespace detail
{
// mx_cell_noise_vec3(vec3): the 4-arg hash structure (seed len=4, +x/+y/+z, bjmix, then bjfinal(a+0/1/2, b, c)). Fills out[3].
inline void cell_vec3_from3(KGraph& g, int ix, int iy, int iz, int like, int out[3])
{
    int a = add32(g, seed_u(g, ix, 4U), to_u(g, ix));
    int b = add32(g, seed_u(g, iy, 4U), to_u(g, iy));
    int c = add32(g, seed_u(g, iz, 4U), to_u(g, iz));
    bjmix(g, a, b, c);
    const int r0 = bjfinal(g, a, b, c);
    const int r1 = bjfinal(g, add32(g, a, ku(g, a, 1U)), b, c);
    const int r2 = bjfinal(g, add32(g, a, ku(g, a, 2U)), b, c);
    const int m  = kf(g, like, static_cast<crd::f64>(kU32Max));
    out[0] = g.binary(KOp::Div, g.cast(r0, g.node(like).dtype()), m);
    out[1] = g.binary(KOp::Div, g.cast(r1, g.node(like).dtype()), m);
    out[2] = g.binary(KOp::Div, g.cast(r2, g.node(like).dtype()), m);
}
// mx_worley_cell_position offset (3D): ((cell_noise_vec3(vec3) - 0.5) * jitter + 0.5).
[[nodiscard]] inline int worley_off3(KGraph& g, int ax, int ay, int az, double jitter, int like)
{
    int cn[3];
    cell_vec3_from3(g, ax, ay, az, like, cn);
    const int off  = g.vec3(cn[0], cn[1], cn[2]);
    const int half = g.splat(kf(g, like, 0.5), 3);
    const int jit  = g.splat(kf(g, like, jitter), 3);
    return g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Sub, off, half), jit), half);
}
} // namespace detail

[[nodiscard]] inline int worley3(KGraph& g, int px, int py, int pz, double jitter, int style, int metric)
{
    namespace d = detail;
    int        xi = 0;
    int        yi = 0;
    int        zi = 0;
    const int  localpos = g.vec3(d::floorfrac(g, px, xi), d::floorfrac(g, py, yi), d::floorfrac(g, pz, zi));
    int        best     = d::kf(g, px, 1e6);
    int        minpos   = g.vec3(d::kf(g, px, 0.0), d::kf(g, px, 0.0), d::kf(g, px, 0.0));
    const auto ci       = [&](int base, int o) { return g.binary(KOp::Add, base, g.constant(static_cast<double>(o), g.node(base).shape, DType::I32)); };
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int z = -1; z <= 1; ++z)
            {
                const int cell = g.binary(KOp::Add, g.vec3(d::kf(g, px, static_cast<double>(x)), d::kf(g, px, static_cast<double>(y)), d::kf(g, px, static_cast<double>(z))), d::worley_off3(g, ci(xi, x), ci(yi, y), ci(zi, z), jitter, px));
                const int diff = g.binary(KOp::Sub, cell, localpos);
                const int dist = d::worley_metric(g, diff, metric, 3);
                const int cond = g.binary(KOp::CmpLt, dist, best);
                best           = g.select(cond, dist, best);
                if (style == 1) { minpos = g.select(cond, diff, minpos); }
            }
        }
    }
    if (style == 1)
    {
        const int mp = g.binary(KOp::Add, minpos, g.vec3(px, py, pz));
        return cell3(g, g.swizzle(mp, 0), g.swizzle(mp, 1), g.swizzle(mp, 2));
    }
    return metric == 0 ? g.unary(KOp::Sqrt, best) : best;
}

} // namespace crd::kir::nodes::noise
