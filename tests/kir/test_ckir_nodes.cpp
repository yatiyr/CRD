// test_ckir_nodes.cpp — D-007 B6-a: the MaterialX-parity OPERATOR node library (crd::kir::nodes) proven BIT-EXACT against
// the MaterialX reference on CKIR's deterministic CPU oracle. Each node is built as a CKIR value graph over N samples and
// eval_cpu'd; the result is compared with `==` (exact) against a hand-transcription of the SAME MaterialX reference formula
// computed in f64. The graphs run in F64 (the node library derives its constants' dtype from the operands, so no F32
// rounding intrudes) — this isolates the OPERATION STRUCTURE (did we transcribe MaterialX's op order/factoring correctly?),
// which is the real conformance risk. The GPU observable (F32, both backends) rides tests/gpu-context-*. Reference sources
// are cited per node in ckir_nodes.hpp (MaterialX main, libraries/stdlib).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_nodes.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;
namespace nd  = crd::kir::nodes;

namespace
{
constexpr int kN = 20;

// exact f64 helpers mirroring the oracle's apply_unary (so the reference matches the node bit-for-bit).
crd::f64 rabs(crd::f64 x) { return x < 0.0 ? -x : x; }
crd::f64 rfloor(crd::f64 x) { return crd::math::floor(x); }
crd::f64 rmin(crd::f64 a, crd::f64 b) { return a < b ? a : b; }
crd::f64 rmax(crd::f64 a, crd::f64 b) { return a > b ? a : b; }
crd::f64 rclamp(crd::f64 x, crd::f64 lo, crd::f64 hi) { const crd::f64 m = x > lo ? x : lo; return m < hi ? m : hi; }
crd::f64 rmix(crd::f64 a, crd::f64 b, crd::f64 t) { return a * (1.0 - t) + b * t; } // KOp::Mix
// pick component `c` of a 3-vector (avoids nested conditional operators the tidy gate forbids).
crd::f64 pick3(int c, crd::f64 a, crd::f64 b, crd::f64 d)
{
    if (c == 0) { return a; }
    if (c == 1) { return b; }
    return d;
}
crd::f64 rsmoothstep(crd::f64 e0, crd::f64 e1, crd::f64 x)
{
    const crd::f64 u  = (x - e0) / (e1 - e0);
    const crd::f64 hi = u > 1.0 ? 1.0 : u;
    const crd::f64 t  = u < 0.0 ? 0.0 : hi;
    return t * t * (3.0 - 2.0 * t);
}
} // namespace

TEST_CASE("B6-a: nodes math + geometric bit-exact vs the MaterialX reference on the CPU oracle", "[kir][nodes][math]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int a  = g.input(sh, kir::DType::F64); // general scalar, spans negatives
    const int b  = g.input(sh, kir::DType::F64); // positive scalar (div/pow/mod/log/sqrt safe)
    const int rx = g.input(sh, kir::DType::F64); // (0,1) — asin/acos/ln domain + a colour channel
    const int gy = g.input(sh, kir::DType::F64);
    const int bz = g.input(sh, kir::DType::F64);

    crd::f64 av[kN];
    crd::f64 bv[kN];
    crd::f64 rv[kN];
    crd::f64 gv[kN];
    crd::f64 zv[kN];
    for (int i = 0; i < kN; ++i)
    {
        av[i] = (0.31 * i) - 3.0;         // [-3, +2.89]
        bv[i] = (0.17 * i) + 0.6;         // [0.6, 3.83]
        rv[i] = (0.045 * i) + 0.05;       // [0.05, 0.905]
        gv[i] = (0.9 - 0.04 * i) + 0.001; // varied
        zv[i] = (0.03 * i) + 0.08;        // [0.08, 0.65]
    }
    const crd::f64* inp[] = {av, bv, rv, gv, zv};

    const int rgb  = g.vec3(rx, gy, bz);
    const int brgb = g.vec3(bz, rx, gy);

    int bad = 0;
    const auto chk = [&](int node, auto ref) { crd::f64 o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, int nc, auto ref) { crd::f64 o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < nc; ++c) { if (o[i * nc + c] != ref(i, c)) { ++bad; } } } };

    chk(nd::add(g, a, b), [&](int i) { return av[i] + bv[i]; });
    chk(nd::subtract(g, a, b), [&](int i) { return av[i] - bv[i]; });
    chk(nd::multiply(g, a, b), [&](int i) { return av[i] * bv[i]; });
    chk(nd::divide(g, a, b), [&](int i) { return av[i] / bv[i]; });
    chk(nd::modulo(g, a, b), [&](int i) { return av[i] - bv[i] * rfloor(av[i] / bv[i]); }); // FLOOR-mod (not fmod)
    chk(nd::invert(g, a), [&](int i) { return 1.0 - av[i]; });
    chk(nd::invert(g, a, b), [&](int i) { return bv[i] - av[i]; });
    chk(nd::absval(g, a), [&](int i) { return rabs(av[i]); });
    chk(nd::floor(g, a), [&](int i) { return crd::math::floor(av[i]); });
    chk(nd::ceil(g, a), [&](int i) { return crd::math::ceil(av[i]); });
    chk(nd::round(g, a), [&](int i) { return crd::math::nearbyint(av[i]); });
    chk(nd::sign(g, a), [&](int i) { return (av[i] > 0.0 ? 1.0 : 0.0) - (av[i] < 0.0 ? 1.0 : 0.0); });
    chk(nd::power(g, rx, b), [&](int i) { return crd::math::pow(rv[i], bv[i]); });
    chk(nd::sqrt(g, rx), [&](int i) { return crd::math::sqrt(rv[i]); });
    chk(nd::ln(g, rx), [&](int i) { return crd::math::log(rv[i]); });
    chk(nd::exp(g, rx), [&](int i) { return crd::math::exp(rv[i]); });
    chk(nd::sin(g, a), [&](int i) { return crd::math::sin(av[i]); });
    chk(nd::cos(g, a), [&](int i) { return crd::math::cos(av[i]); });
    chk(nd::tan(g, rx), [&](int i) { return crd::math::tan(rv[i]); });
    chk(nd::asin(g, rx), [&](int i) { return crd::math::asin(rv[i]); });
    chk(nd::acos(g, rx), [&](int i) { return crd::math::acos(rv[i]); });
    chk(nd::atan2(g, a, b), [&](int i) { return crd::math::atan2(av[i], bv[i]); });
    chk(nd::minimum(g, a, b), [&](int i) { return rmin(av[i], bv[i]); });
    chk(nd::maximum(g, a, b), [&](int i) { return rmax(av[i], bv[i]); });
    chk(nd::clamp(g, a, rx, b), [&](int i) { return rclamp(av[i], rv[i], bv[i]); });
    chk(nd::smoothstep(g, a, rx, b), [&](int i) { return rsmoothstep(rv[i], bv[i], av[i]); });
    chk(nd::remap(g, a, rx, b, bz, gy), // remap(in=a, inlow=rx, inhigh=b, outlow=bz, outhigh=gy)
        [&](int i) { const crd::f64 t = (av[i] - rv[i]) / (bv[i] - rv[i]); return zv[i] + t * (gv[i] - zv[i]); });

    // geometric
    chk(nd::magnitude(g, rgb), [&](int i) { return crd::math::sqrt(rv[i] * rv[i] + gv[i] * gv[i] + zv[i] * zv[i]); });
    chk(nd::dotproduct(g, rgb, brgb), [&](int i) { return rv[i] * zv[i] + gv[i] * rv[i] + zv[i] * gv[i]; });
    chkv(nd::normalize(g, rgb), 3, [&](int i, int c) { const crd::f64 l = crd::math::sqrt(rv[i] * rv[i] + gv[i] * gv[i] + zv[i] * zv[i]); return pick3(c, rv[i], gv[i], zv[i]) / l; });
    chkv(nd::crossproduct(g, rgb, brgb), 3, [&](int i, int c) { const crd::f64 ax = rv[i]; const crd::f64 ay = gv[i]; const crd::f64 az = zv[i]; const crd::f64 bx = zv[i]; const crd::f64 by = rv[i]; const crd::f64 bcz = gv[i]; return pick3(c, ay * bcz - az * by, az * bx - ax * bcz, ax * by - ay * bx); });
    chk(nd::distance(g, rgb, brgb), [&](int i) { const crd::f64 dx = rv[i] - zv[i]; const crd::f64 dy = gv[i] - rv[i]; const crd::f64 dz = zv[i] - gv[i]; return crd::math::sqrt(dx * dx + dy * dy + dz * dz); });

    CHECK(bad == 0);
}

TEST_CASE("B6-a: nodes adjustment (luminance/contrast/range/saturate/hsv) bit-exact vs MaterialX", "[kir][nodes][adjustment]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int rx = g.input(sh, kir::DType::F64);
    const int gy = g.input(sh, kir::DType::F64);
    const int bz = g.input(sh, kir::DType::F64);
    const int am = g.input(sh, kir::DType::F64); // amount / mix

    crd::f64 rv[kN];
    crd::f64 gv[kN];
    crd::f64 zv[kN];
    crd::f64 amv[kN];
    for (int i = 0; i < kN; ++i)
    {
        rv[i]  = (0.045 * i) + 0.05; // [0.05, 0.905]
        gv[i]  = 0.9 - (0.03 * i);   // decreasing, distinct from r/b
        zv[i]  = (0.02 * i) + 0.1;   // [0.10, 0.48]
        amv[i] = (0.04 * i) + 0.15;  // [0.15, 0.91]
    }
    const crd::f64* inp[] = {rv, gv, zv, amv};

    const int rgb = g.vec3(rx, gy, bz);

    int bad = 0;
    const auto chkv = [&](int node, int nc, auto ref) { crd::f64 o[kN * 4]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < nc; ++c) { if (o[i * nc + c] != ref(i, c)) { ++bad; } } } };

    // luminance: vec3(dot(rgb, ACEScg lumacoeffs)) — all 3 channels equal.
    chkv(nd::luminance(g, rgb), 3, [&](int i, int /*c*/) { return rv[i] * nd::kLumaR + gv[i] * nd::kLumaG + zv[i] * nd::kLumaB; });
    // contrast(in, amount, pivot=0.5): (in - pivot)*amount + pivot.
    {
        const int piv = g.constant(0.5, sh, kir::DType::F64);
        chkv(nd::contrast(g, rgb, am, piv), 3, [&](int i, int c) { return (pick3(c, rv[i], gv[i], zv[i]) - 0.5) * amv[i] + 0.5; });
    }
    // saturate(in, amount) = mix(luma, in, amount).
    chkv(nd::saturate(g, rgb, am), 3, [&](int i, int c) { const crd::f64 luma = rv[i] * nd::kLumaR + gv[i] * nd::kLumaG + zv[i] * nd::kLumaB; return rmix(luma, pick3(c, rv[i], gv[i], zv[i]), amv[i]); });
    // rgbtohsv (branchy reference; delta>0 here so no NaN arm).
    const auto ref_rgbtohsv = [&](int i, int c)
    {
        const crd::f64 r = rv[i];
        const crd::f64 gg = gv[i];
        const crd::f64 b = zv[i];
        const crd::f64 mx = rmax(r, rmax(gg, b));
        const crd::f64 mn = rmin(r, rmin(gg, b));
        const crd::f64 delta = mx - mn;
        const crd::f64 v = mx;
        const crd::f64 s = mx > 0.0 ? delta / mx : 0.0;
        crd::f64       h = 0.0;
        if (r >= mx) { h = (gg - b) / delta; }
        else if (gg >= mx) { h = 2.0 + (b - r) / delta; }
        else { h = 4.0 + (r - gg) / delta; }
        h *= (1.0 / 6.0);
        if (h < 0.0) { h += 1.0; }
        if (s <= 0.0) { h = 0.0; }
        return pick3(c, h, s, v);
    };
    chkv(nd::rgbtohsv(g, rgb), 3, ref_rgbtohsv);
    // hsvtorgb.
    const auto ref_hsvtorgb = [&](crd::f64 h, crd::f64 s, crd::f64 v, int c)
    {
        if (s < 0.0001) { return v; }
        const crd::f64 h6 = 6.0 * (h - rfloor(h));
        const crd::f64 hi = crd::math::trunc(h6);
        const crd::f64 f  = h6 - hi;
        const crd::f64 p  = v * (1.0 - s);
        const crd::f64 q  = v * (1.0 - s * f);
        const crd::f64 t  = v * (1.0 - s * (1.0 - f));
        if (hi == 0.0) { return pick3(c, v, t, p); }
        if (hi == 1.0) { return pick3(c, q, v, p); }
        if (hi == 2.0) { return pick3(c, p, v, t); }
        if (hi == 3.0) { return pick3(c, p, q, v); }
        if (hi == 4.0) { return pick3(c, t, p, v); }
        return pick3(c, v, p, q);
    };
    {
        const int hsv = g.vec3(rx, am, gy); // h=rx in [0.05,0.9], s=am in [0.15,0.9], v=gy
        chkv(nd::hsvtorgb(g, hsv), 3, [&](int i, int c) { return ref_hsvtorgb(rv[i], amv[i], gv[i], c); });
    }

    CHECK(bad == 0);
}

TEST_CASE("B6-a: nodes compositing (blend + Porter-Duff) bit-exact vs MaterialX", "[kir][nodes][compositing]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    // fg / bg colour4 channels + mix — kept in (0,1) and away from the burn/dodge/disjointover guard singularities.
    const int fr = g.input(sh, kir::DType::F64);
    const int fg = g.input(sh, kir::DType::F64);
    const int fb = g.input(sh, kir::DType::F64);
    const int fa = g.input(sh, kir::DType::F64);
    const int br = g.input(sh, kir::DType::F64);
    const int bg = g.input(sh, kir::DType::F64);
    const int bb = g.input(sh, kir::DType::F64);
    const int ba = g.input(sh, kir::DType::F64);
    const int mx = g.input(sh, kir::DType::F64);

    crd::f64 v[9][kN];
    for (int i = 0; i < kN; ++i)
    {
        v[0][i] = 0.10 + 0.03 * i;  // fr [0.10, 0.67]
        v[1][i] = 0.20 + 0.02 * i;  // fg
        v[2][i] = 0.35 + 0.01 * i;  // fb
        v[3][i] = 0.20 + 0.025 * i; // fa [0.20, 0.675]
        v[4][i] = 0.55 - 0.01 * i;  // br
        v[5][i] = 0.30 + 0.015 * i; // bg
        v[6][i] = 0.45 - 0.005 * i; // bb
        v[7][i] = 0.25 + 0.02 * i;  // ba
        v[8][i] = 0.15 + 0.03 * i;  // mix [0.15, 0.72]
    }
    const crd::f64* inp[] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]};

    const int fg4 = g.vec4(fr, fg, fb, fa);
    const int bg4 = g.vec4(br, bg, bb, ba);

    int bad = 0;
    const auto chk4 = [&](int node, auto basef) { crd::f64 o[kN * 4]; kir::eval_cpu(g, inp, &alloc, node, o);
        for (int i = 0; i < kN; ++i) { for (int c = 0; c < 4; ++c) { const crd::f64 bgc = v[4 + c][i]; const crd::f64 want = rmix(bgc, basef(i, c), v[8][i]); if (o[i * 4 + c] != want) { ++bad; } } } };
    const auto fch = [&](int i, int c) { return v[c][i]; };     // fg channel c
    const auto bch = [&](int i, int c) { return v[4 + c][i]; }; // bg channel c

    // per-channel blends (operator applied to all 4 channels, then the mix tail).
    chk4(nd::plus(g, fg4, bg4, mx), [&](int i, int c) { return fch(i, c) + bch(i, c); });
    chk4(nd::minus(g, fg4, bg4, mx), [&](int i, int c) { return bch(i, c) - fch(i, c); });
    chk4(nd::difference(g, fg4, bg4, mx), [&](int i, int c) { return rabs(fch(i, c) - bch(i, c)); });
    chk4(nd::screen(g, fg4, bg4, mx), [&](int i, int c) { return 1.0 - (1.0 - fch(i, c)) * (1.0 - bch(i, c)); });
    chk4(nd::overlay(g, fg4, bg4, mx), [&](int i, int c) { const crd::f64 f = fch(i, c); const crd::f64 b = bch(i, c); return b >= 0.5 ? (1.0 - 2.0 * (1.0 - b) * (1.0 - f)) : (2.0 * f * b); });
    chk4(nd::burn(g, fg4, bg4, mx), [&](int i, int c) { const crd::f64 f = fch(i, c); const crd::f64 b = bch(i, c); return rabs(f) < 1e-8 ? 0.0 : 1.0 - (1.0 - b) / f; });
    chk4(nd::dodge(g, fg4, bg4, mx), [&](int i, int c) { const crd::f64 f = fch(i, c); const crd::f64 b = bch(i, c); return rabs(1.0 - f) < 1e-8 ? 0.0 : b / (1.0 - f); });

    // Porter-Duff (rgb uses alpha; alpha composited separately), then the mix tail.
    const auto pd = [&](int node, auto rgbf, auto af) { crd::f64 o[kN * 4]; kir::eval_cpu(g, inp, &alloc, node, o);
        for (int i = 0; i < kN; ++i) { for (int c = 0; c < 4; ++c) { const crd::f64 base = c < 3 ? rgbf(i, c) : af(i); const crd::f64 want = rmix(v[4 + c][i], base, v[8][i]); if (o[i * 4 + c] != want) { ++bad; } } } };
    pd(nd::over(g, fg4, bg4, mx), [&](int i, int c) { return fch(i, c) + bch(i, c) * (1.0 - v[3][i]); }, [&](int i) { return v[3][i] + v[7][i] * (1.0 - v[3][i]); });
    pd(nd::comp_in(g, fg4, bg4, mx), [&](int i, int c) { return fch(i, c) * v[7][i]; }, [&](int i) { return v[3][i] * v[7][i]; });
    pd(nd::comp_out(g, fg4, bg4, mx), [&](int i, int c) { return fch(i, c) * (1.0 - v[7][i]); }, [&](int i) { return v[3][i] * (1.0 - v[7][i]); });
    pd(nd::mask(g, fg4, bg4, mx), [&](int i, int c) { return bch(i, c) * v[3][i]; }, [&](int i) { return v[7][i] * v[3][i]; });
    pd(nd::matte(g, fg4, bg4, mx), [&](int i, int c) { return fch(i, c) * v[3][i] + bch(i, c) * (1.0 - v[3][i]); }, [&](int i) { return v[3][i] + v[7][i] * (1.0 - v[3][i]); });

    // disjointover (summed-alpha; f+b<=1 branch vs the >1 branch, both exercised across samples).
    {
        crd::f64 o[kN * 4];
        kir::eval_cpu(g, inp, &alloc, nd::disjointover(g, fg4, bg4, mx), o);
        for (int i = 0; i < kN; ++i)
        {
            const crd::f64 summed = v[3][i] + v[7][i];
            crd::f64       base[4];
            for (int c = 0; c < 3; ++c)
            {
                if (summed <= 1.0) { base[c] = fch(i, c) + bch(i, c); }
                else if (rabs(v[7][i]) < 1e-8) { base[c] = 0.0; }
                else { base[c] = fch(i, c) + bch(i, c) * ((1.0 - v[3][i]) / v[7][i]); }
            }
            base[3] = rmin(summed, 1.0);
            for (int c = 0; c < 4; ++c) { const crd::f64 want = rmix(v[4 + c][i], base[c], v[8][i]); if (o[i * 4 + c] != want) { ++bad; } }
        }
    }
    // mix / premult / unpremult.
    chk4(nd::mix(g, fg4, bg4, mx), [&](int i, int c) { return fch(i, c); }); // mix base = fg, then mix tail == MaterialX mix
    {
        crd::f64 o[kN * 4];
        kir::eval_cpu(g, inp, &alloc, nd::premult(g, fg4), o);
        for (int i = 0; i < kN; ++i) { for (int c = 0; c < 4; ++c) { const crd::f64 want = c < 3 ? fch(i, c) * v[3][i] : v[3][i]; if (o[i * 4 + c] != want) { ++bad; } } }
        kir::eval_cpu(g, inp, &alloc, nd::unpremult(g, fg4), o);
        for (int i = 0; i < kN; ++i) { for (int c = 0; c < 4; ++c) { const crd::f64 want = c < 3 ? fch(i, c) / v[3][i] : v[3][i]; if (o[i * 4 + c] != want) { ++bad; } } }
    }

    CHECK(bad == 0);
}

TEST_CASE("B6-a: nodes logical + conditional + channel bit-exact vs MaterialX", "[kir][nodes][conditional]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int la  = g.input(sh, kir::DType::F64); // boolean 0/1
    const int lb  = g.input(sh, kir::DType::F64); // boolean 0/1
    const int v1  = g.input(sh, kir::DType::F64); // compare operand
    const int v2  = g.input(sh, kir::DType::F64);
    const int wch = g.input(sh, kir::DType::F64); // switch selector 0..4

    crd::f64 lav[kN];
    crd::f64 lbv[kN];
    crd::f64 v1v[kN];
    crd::f64 v2v[kN];
    crd::f64 wv[kN];
    for (int i = 0; i < kN; ++i)
    {
        lav[i] = static_cast<crd::f64>(i & 1);
        lbv[i] = static_cast<crd::f64>((i / 2) & 1);
        v1v[i] = static_cast<crd::f64>(i) - 10.0;
        v2v[i] = static_cast<crd::f64>((i * 3) % 7) - 3.0;
        wv[i]  = static_cast<crd::f64>(i % 5);
    }
    const crd::f64* inp[] = {lav, lbv, v1v, v2v, wv};

    int bad = 0;
    const auto chk = [&](int node, auto ref) { crd::f64 o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };

    chk(nd::logical_and(g, la, lb), [&](int i) { return rmin(lav[i], lbv[i]); });
    chk(nd::logical_or(g, la, lb), [&](int i) { return rmax(lav[i], lbv[i]); });
    chk(nd::logical_not(g, la), [&](int i) { return 1.0 - lav[i]; });
    chk(nd::logical_xor(g, la, lb), [&](int i) { return lav[i] != lbv[i] ? 1.0 : 0.0; });

    chk(nd::ifgreater(g, v1, v2, la, lb), [&](int i) { return v1v[i] > v2v[i] ? lav[i] : lbv[i]; });
    chk(nd::ifgreatereq(g, v1, v2, la, lb), [&](int i) { return v1v[i] >= v2v[i] ? lav[i] : lbv[i]; });
    chk(nd::ifequal(g, v1, v2, la, lb), [&](int i) { return v1v[i] == v2v[i] ? lav[i] : lbv[i]; });

    // switch5: selector wch in {0..4} picks constants 10..14.
    {
        const int s0 = g.constant(10.0, sh, kir::DType::F64);
        const int s1 = g.constant(11.0, sh, kir::DType::F64);
        const int s2 = g.constant(12.0, sh, kir::DType::F64);
        const int s3 = g.constant(13.0, sh, kir::DType::F64);
        const int s4 = g.constant(14.0, sh, kir::DType::F64);
        chk(nd::switch5(g, s0, s1, s2, s3, s4, wch), [&](int i) { return 10.0 + wv[i]; });
    }

    // channel: extract + combine round-trip on a vec3.
    {
        const int vecn = g.vec3(v1, v2, la);
        chk(nd::extract(g, vecn, 1), [&](int i) { return v2v[i]; });
        crd::f64 o3[kN * 3];
        kir::eval_cpu(g, inp, &alloc, nd::combine3(g, v1, v2, la), o3);
        for (int i = 0; i < kN; ++i) { if (o3[i * 3] != v1v[i] || o3[i * 3 + 1] != v2v[i] || o3[i * 3 + 2] != lav[i]) { ++bad; } }
    }

    CHECK(bad == 0);
}

TEST_CASE("B6-a: nodes guard branches (burn/dodge edge) bit-exact vs MaterialX", "[kir][nodes][guard]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({4});

    const int fr = g.input(sh, kir::DType::F64);
    const int fg = g.input(sh, kir::DType::F64);
    const int fb = g.input(sh, kir::DType::F64);
    const int fa = g.input(sh, kir::DType::F64);
    const int br = g.input(sh, kir::DType::F64);
    const int bg = g.input(sh, kir::DType::F64);
    const int bb = g.input(sh, kir::DType::F64);
    const int ba = g.input(sh, kir::DType::F64);
    const int mx = g.input(sh, kir::DType::F64);

    // sample 0: fg=0 (burn guard fires) · sample 1: fg=1 (dodge guard fires) · samples 2,3: normal.
    crd::f64        fv[4]  = {0.0, 1.0, 0.4, 0.6};
    crd::f64        bv[4]  = {0.5, 0.5, 0.3, 0.7};
    crd::f64        av[4]  = {1.0, 1.0, 1.0, 1.0};
    crd::f64        mv[4]  = {1.0, 1.0, 1.0, 1.0}; // mix=1 → result == base
    const crd::f64* inp[]  = {fv, fv, fv, av, bv, bv, bv, av, mv};

    const int fg4 = g.vec4(fr, fg, fb, fa);
    const int bg4 = g.vec4(br, bg, bb, ba);

    int bad = 0;
    {
        crd::f64 o[4 * 4];
        kir::eval_cpu(g, inp, &alloc, nd::burn(g, fg4, bg4, mx), o);
        for (int i = 0; i < 4; ++i) { const crd::f64 want = rabs(fv[i]) < 1e-8 ? 0.0 : 1.0 - (1.0 - bv[i]) / fv[i]; if (o[i * 4] != want) { ++bad; } } // channel 0
        kir::eval_cpu(g, inp, &alloc, nd::dodge(g, fg4, bg4, mx), o);
        for (int i = 0; i < 4; ++i) { const crd::f64 want = rabs(1.0 - fv[i]) < 1e-8 ? 0.0 : bv[i] / (1.0 - fv[i]); if (o[i * 4] != want) { ++bad; } }
    }
    CHECK(bad == 0);
}

TEST_CASE("B6-b: nodes shapes (ramplr/ramptb/checkerboard) bit-exact vs MaterialX", "[kir][nodes][shapes]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int vl = g.input(sh, kir::DType::F64);
    const int vr = g.input(sh, kir::DType::F64);
    const int tx = g.input(sh, kir::DType::F64);
    const int ty = g.input(sh, kir::DType::F64);

    crd::f64 vlv[kN];
    crd::f64 vrv[kN];
    crd::f64 txv[kN];
    crd::f64 tyv[kN];
    for (int i = 0; i < kN; ++i)
    {
        vlv[i] = 0.1 + 0.03 * i;
        vrv[i] = 0.9 - 0.02 * i;
        txv[i] = (0.09 * i) - 0.3; // spans <0, [0,1], >1 → exercises the clamp
        tyv[i] = (0.11 * i) - 0.5;
    }
    const crd::f64* inp[] = {vlv, vrv, txv, tyv};

    const int tc = g.vec2(tx, ty);

    int        bad = 0;
    const auto chk = [&](int node, auto ref) { crd::f64 o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };

    chk(nd::ramplr(g, vl, vr, tc), [&](int i) { const crd::f64 t = rclamp(txv[i], 0.0, 1.0); return rmix(vlv[i], vrv[i], t); });
    chk(nd::ramptb(g, vl, vr, tc), [&](int i) { const crd::f64 t = rclamp(tyv[i], 0.0, 1.0); return rmix(vrv[i], vlv[i], t); }); // valueb=vr, valuet=vl
    // checkerboard with scalar colours, tiling (2,2), offset (0,0): mod(floor(2tx)+floor(2ty), 2) selects vr/vl.
    {
        const int tiling = g.vec2(g.constant(2.0, sh, kir::DType::F64), g.constant(2.0, sh, kir::DType::F64));
        const int offset = g.vec2(g.constant(0.0, sh, kir::DType::F64), g.constant(0.0, sh, kir::DType::F64));
        chk(nd::checkerboard(g, vl, vr, tiling, offset, tc), [&](int i) {
            const crd::f64 d  = rfloor(2.0 * txv[i]) + rfloor(2.0 * tyv[i]);
            const crd::f64 md = d - 2.0 * rfloor(d / 2.0);
            return rmix(vrv[i], vlv[i], md); // color2=vr, color1=vl → vr*(1-md)+vl*md
        });
    }

    CHECK(bad == 0);
}

TEST_CASE("B6-c: nodes UV (rotate2d/rotate3d/place2d/triplanar) bit-exact vs MaterialX", "[kir][nodes][uv]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int tx = g.input(sh, kir::DType::F64);
    const int ty = g.input(sh, kir::DType::F64);
    const int am = g.input(sh, kir::DType::F64); // rotate degrees
    const int nx = g.input(sh, kir::DType::F64);
    const int ny = g.input(sh, kir::DType::F64);
    const int nz = g.input(sh, kir::DType::F64);
    const int bl = g.input(sh, kir::DType::F64); // triplanar blend

    crd::f64 txv[kN];
    crd::f64 tyv[kN];
    crd::f64 amv[kN];
    crd::f64 nxv[kN];
    crd::f64 nyv[kN];
    crd::f64 nzv[kN];
    crd::f64 blv[kN];
    for (int i = 0; i < kN; ++i)
    {
        txv[i] = (0.09 * i) - 0.4;
        tyv[i] = (0.07 * i) + 0.2;
        amv[i] = (13.0 * i) - 90.0;      // degrees, spans negatives
        nxv[i] = (0.05 * i) - 0.5;
        nyv[i] = 0.8 - (0.03 * i);
        nzv[i] = (0.04 * i) + 0.3;       // keep the normal non-degenerate
        blv[i] = (0.05 * i) + 0.1;       // [0.1, 1.05] → exercises the 0.03/1 clamp
    }
    const crd::f64* inp[] = {txv, tyv, amv, nxv, nyv, nzv, blv};

    const int tc  = g.vec2(tx, ty);
    const int v3  = g.vec3(nx, ny, nz);
    const int axs = g.vec3(nz, nx, ny); // a non-degenerate axis (permuted)

    constexpr double deg2rad = 0.017453292519943295; // KOp::Radians constant
    const auto       dnorm3 = [](crd::f64 x, crd::f64 y, crd::f64 z) { return crd::math::sqrt(x * x + y * y + z * z); };

    int        bad = 0;
    const auto chkv = [&](int node, int nc, auto ref) { crd::f64 o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < nc; ++c) { if (o[i * nc + c] != ref(i, c)) { ++bad; } } } };
    const auto chk  = [&](int node, auto ref) { crd::f64 o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };

    // rotate2d: (ca*x+sa*y, -sa*x+ca*y).
    const auto rot2 = [&](int i, int c) { const crd::f64 rad = amv[i] * deg2rad; const crd::f64 sa = crd::math::sin(rad); const crd::f64 ca = crd::math::cos(rad); return c == 0 ? (ca * txv[i] + sa * tyv[i]) : (-sa * txv[i] + ca * tyv[i]); };
    chkv(nd::rotate2d(g, tc, am), 2, rot2);

    // rotate3d (Rodrigues): in*c + cross(in,n)*s + n*dot(n,in)*(1-c), n = normalize(axis).
    chkv(nd::rotate3d(g, v3, am, axs), 3, [&](int i, int c) {
        const crd::f64 al = dnorm3(nzv[i], nxv[i], nyv[i]); // axis = (nz, nx, ny)
        const crd::f64 ax = nzv[i] / al;
        const crd::f64 ay = nxv[i] / al;
        const crd::f64 az = nyv[i] / al;
        const crd::f64 ix = nxv[i];
        const crd::f64 iy = nyv[i];
        const crd::f64 iz = nzv[i];
        const crd::f64 rad = amv[i] * deg2rad;
        const crd::f64 s = crd::math::sin(rad);
        const crd::f64 cc = crd::math::cos(rad);
        const crd::f64 oc = 1.0 - cc;
        const crd::f64 dt = ax * ix + ay * iy + az * iz;
        const crd::f64 cx = iy * az - iz * ay; // cross(in, axis)
        const crd::f64 cy = iz * ax - ix * az;
        const crd::f64 cz = ix * ay - iy * ax;
        return pick3(c, ix * cc + cx * s + ax * dt * oc, iy * cc + cy * s + ay * dt * oc, iz * cc + cz * s + az * dt * oc);
    });

    // place2d SRT (order 0): rotate2d((tc-pivot)/scale, rot) - offset + pivot, pivot=(0.5,0.5) scale=(2,2) offset=(0.1,0.2).
    {
        const int piv = g.vec2(g.constant(0.5, sh, kir::DType::F64), g.constant(0.5, sh, kir::DType::F64));
        const int scl = g.vec2(g.constant(2.0, sh, kir::DType::F64), g.constant(2.0, sh, kir::DType::F64));
        const int ofs = g.vec2(g.constant(0.1, sh, kir::DType::F64), g.constant(0.2, sh, kir::DType::F64));
        chkv(nd::place2d(g, tc, piv, scl, am, ofs, 0), 2, [&](int i, int c) {
            const crd::f64 sx = (txv[i] - 0.5) / 2.0;
            const crd::f64 sy = (tyv[i] - 0.5) / 2.0;
            const crd::f64 rad = amv[i] * deg2rad;
            const crd::f64 sa = crd::math::sin(rad);
            const crd::f64 ca = crd::math::cos(rad);
            const crd::f64 rx = ca * sx + sa * sy;
            const crd::f64 ry = -sa * sx + ca * sy;
            return c == 0 ? (rx - 0.1 + 0.5) : (ry - 0.2 + 0.5);
        });
    }

    // triplanar_weights: normalize(abs(normalize(n))), pow(1/clamp(blend,0.03,1)), re-normalize.
    chkv(nd::triplanar_weights(g, v3, bl), 3, [&](int i, int c) {
        const crd::f64 l = dnorm3(nxv[i], nyv[i], nzv[i]);
        crd::f64       a0 = rabs(nxv[i] / l);
        crd::f64       a1 = rabs(nyv[i] / l);
        crd::f64       a2 = rabs(nzv[i] / l);
        const crd::f64 sa = a0 + a1 + a2;
        a0 /= sa;
        a1 /= sa;
        a2 /= sa;
        const crd::f64 inv = 1.0 / rclamp(blv[i], 0.03, 1.0);
        const crd::f64 p0 = crd::math::pow(a0, inv);
        const crd::f64 p1 = crd::math::pow(a1, inv);
        const crd::f64 p2 = crd::math::pow(a2, inv);
        const crd::f64 sp = p0 + p1 + p2;
        return pick3(c, p0 / sp, p1 / sp, p2 / sp);
    });

    // triplanar blend: sx*wx + sy*wy + sz*wz.
    {
        const int w = nd::triplanar_weights(g, v3, bl);
        chk(nd::triplanar(g, nx, ny, nz, w), [&](int i) {
            const crd::f64 l = dnorm3(nxv[i], nyv[i], nzv[i]);
            crd::f64       a0 = rabs(nxv[i] / l);
            crd::f64       a1 = rabs(nyv[i] / l);
            crd::f64       a2 = rabs(nzv[i] / l);
            const crd::f64 sa = a0 + a1 + a2;
            a0 /= sa;
            a1 /= sa;
            a2 /= sa;
            const crd::f64 inv = 1.0 / rclamp(blv[i], 0.03, 1.0);
            const crd::f64 p0 = crd::math::pow(a0, inv);
            const crd::f64 p1 = crd::math::pow(a1, inv);
            const crd::f64 p2 = crd::math::pow(a2, inv);
            const crd::f64 sp = p0 + p1 + p2;
            return nxv[i] * (p0 / sp) + nyv[i] * (p1 / sp) + nzv[i] * (p2 / sp);
        });
    }

    CHECK(bad == 0);
}

TEST_CASE("B6-d: nodes NPR (facingratio / gooch_shade) bit-exact vs MaterialX", "[kir][nodes][npr]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int nx = g.input(sh, kir::DType::F64);
    const int ny = g.input(sh, kir::DType::F64);
    const int nz = g.input(sh, kir::DType::F64);
    const int vx = g.input(sh, kir::DType::F64);
    const int vy = g.input(sh, kir::DType::F64);
    const int vz = g.input(sh, kir::DType::F64);
    const int sp = g.input(sh, kir::DType::F64); // shininess

    crd::f64 nxv[kN];
    crd::f64 nyv[kN];
    crd::f64 nzv[kN];
    crd::f64 vxv[kN];
    crd::f64 vyv[kN];
    crd::f64 vzv[kN];
    crd::f64 spv[kN];
    for (int i = 0; i < kN; ++i)
    {
        nxv[i] = (0.06 * i) - 0.6;
        nyv[i] = 0.7 - (0.04 * i);
        nzv[i] = (0.05 * i) + 0.4;
        vxv[i] = (0.03 * i) - 0.3;
        vyv[i] = (0.02 * i) - 0.2;
        vzv[i] = (0.04 * i) + 0.5;
        spv[i] = (3.0 * i) + 4.0; // shininess
    }
    const crd::f64* inp[] = {nxv, nyv, nzv, vxv, vyv, vzv, spv};

    const int nrm  = g.vec3(nx, ny, nz);
    const int view = g.vec3(vx, vy, vz);

    // exact-order helpers matching the oracle's vector ops.
    const auto dnorm3 = [](crd::f64 x, crd::f64 y, crd::f64 z) { return crd::math::sqrt(x * x + y * y + z * z); };
    const auto dot3   = [](crd::f64 ax, crd::f64 ay, crd::f64 az, crd::f64 bx, crd::f64 by, crd::f64 bz) { return ax * bx + ay * by + az * bz; };

    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { crd::f64 o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, int nc, auto ref) { crd::f64 o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < nc; ++c) { if (o[i * nc + c] != ref(i, c)) { ++bad; } } } };

    // facingratio, all four (faceforward, invert) combinations.
    const auto fr = [&](int i, bool ff, bool inv) { const crd::f64 d = dot3(vxv[i], vyv[i], vzv[i], nxv[i], nyv[i], nzv[i]); crd::f64 f = ff ? rabs(d) : (d * -1.0); return inv ? (1.0 - f) : f; };
    chk(nd::facingratio(g, view, nrm, true, false), [&](int i) { return fr(i, true, false); });
    chk(nd::facingratio(g, view, nrm, false, false), [&](int i) { return fr(i, false, false); });
    chk(nd::facingratio(g, view, nrm, true, true), [&](int i) { return fr(i, true, true); });
    chk(nd::facingratio(g, view, nrm, false, true), [&](int i) { return fr(i, false, true); });

    // gooch_shade: warm=(0.8,0.8,0.7), cool=(0.3,0.3,0.8), spec_intensity=0.9, light=(1,-0.5,-0.5).
    {
        const auto v3c = [&](double a, double b, double c) { return g.vec3(g.constant(a, sh, kir::DType::F64), g.constant(b, sh, kir::DType::F64), g.constant(c, sh, kir::DType::F64)); };
        const int  warm = v3c(0.8, 0.8, 0.7);
        const int  cool = v3c(0.3, 0.3, 0.8);
        const int  spi  = g.constant(0.9, sh, kir::DType::F64);
        const int  ld   = v3c(1.0, -0.5, -0.5);
        const double wc[3] = {0.8, 0.8, 0.7};
        const double cc[3] = {0.3, 0.3, 0.8};
        chkv(nd::gooch_shade(g, nrm, view, warm, cool, spi, sp, ld), 3, [&](int i, int c) {
            const crd::f64 nl = dnorm3(nxv[i], nyv[i], nzv[i]);
            const crd::f64 un0 = nxv[i] / nl;
            const crd::f64 un1 = nyv[i] / nl;
            const crd::f64 un2 = nzv[i] / nl;
            const crd::f64 vl = dnorm3(vxv[i], vyv[i], vzv[i]);
            const crd::f64 uv0 = vxv[i] / vl;
            const crd::f64 uv1 = vyv[i] / vl;
            const crd::f64 uv2 = vzv[i] / vl;
            const crd::f64 ll = dnorm3(1.0, -0.5, -0.5);
            const crd::f64 ul0 = 1.0 / ll;
            const crd::f64 ul1 = -0.5 / ll;
            const crd::f64 ul2 = -0.5 / ll;
            const crd::f64 ndotl = dot3(un0, un1, un2, ul0, ul1, ul2);
            const crd::f64 ci    = (1.0 + ndotl) / 2.0;
            const crd::f64 diffuse = rmix(wc[c], cc[c], ci);
            const crd::f64 dnv = dot3(un0, un1, un2, uv0, uv1, uv2); // reflect(V,N): dp = dot(N,V)
            const crd::f64 rf0 = uv0 - 2.0 * dnv * un0;
            const crd::f64 rf1 = uv1 - 2.0 * dnv * un1;
            const crd::f64 rf2 = uv2 - 2.0 * dnv * un2;
            const crd::f64 vdotr = dot3(-ul0, -ul1, -ul2, rf0, rf1, rf2);
            const crd::f64 nn    = vdotr > 0.0 ? vdotr : 0.0;
            const crd::f64 spec  = crd::math::pow(nn, spv[i]) * 0.9;
            return diffuse + spec;
        });
    }

    CHECK(bad == 0);
}
