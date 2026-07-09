// test_ckir_bitops.cpp — Phase 3.1.6 v17-i: the first geometry technique authored in the CENTRAL IR. CKIR grew integer
// bitwise ops (Shl/Shr/BitAnd/BitOr/BitXor); here we build 3-D Morton-code generation (bit-interleave) entirely as a CKIR
// graph and prove it bit-exact against the hand-written reference on the CPU oracle. This is the proof that LBVH-class
// kernels can be authored ONCE in the shared IR (→ every backend), not hand-written per API. 30-bit codes exceed f32's
// 24-bit mantissa, so the CPU reference runs in f64 (exact); integer GPU storage is the next rung.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm> // std::sort on a C array — the sorted-values oracle (allowed: algorithm, not a std container)
#include <cmath>     // std::floor for the intrinsic reference

namespace kir = crd::kir;

namespace
{
constexpr int kN = 256;

// reference: spread the low 10 bits of v to every 3rd bit position (the standard Morton expand).
crd::u32 expand_ref(crd::u32 v)
{
    v = (v | (v << 16)) & 0x030000FFU;
    v = (v | (v << 8)) & 0x0300F00FU;
    v = (v | (v << 4)) & 0x030C30C3U;
    v = (v | (v << 2)) & 0x09249249U;
    return v;
}
crd::u32 morton_ref(crd::u32 qx, crd::u32 qy, crd::u32 qz)
{
    return (expand_ref(qx) << 2) | (expand_ref(qy) << 1) | expand_ref(qz);
}

// the SAME expand chain, built as a CKIR graph over an I32 node `v` of shape `sh`.
int expand_ckir(kir::KGraph& g, int v, const kir::Shape& sh)
{
    auto konst = [&](crd::i64 c) { return g.constant(static_cast<crd::f64>(c), sh, kir::DType::I32); };
    auto shl   = [&](int a, crd::i64 b) { return g.binary(kir::KOp::Shl, a, konst(b)); };
    auto bor   = [&](int a, int b) { return g.binary(kir::KOp::BitOr, a, b); };
    auto band  = [&](int a, crd::i64 m) { return g.binary(kir::KOp::BitAnd, a, konst(m)); };
    v = band(bor(v, shl(v, 16)), 0x030000FF);
    v = band(bor(v, shl(v, 8)), 0x0300F00F);
    v = band(bor(v, shl(v, 4)), 0x030C30C3);
    v = band(bor(v, shl(v, 2)), 0x09249249);
    return v;
}
} // namespace

TEST_CASE("v17-i: CKIR expresses 3D Morton (bit-interleave), bit-exact vs the reference on the CPU oracle", "[kir][bitops][morton]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int x = g.input(sh, kir::DType::F32);
    const int y = g.input(sh, kir::DType::F32);
    const int z = g.input(sh, kir::DType::F32);

    // quantize a normalized coord to a 10-bit integer: floor(clamp(c*1024, 0, 1023)) -> I32.
    auto konstf   = [&](crd::f64 c) { return g.constant(c, sh, kir::DType::F32); };
    auto quantize = [&](int c) {
        int s = g.binary(kir::KOp::Mul, c, konstf(1024.0));
        s     = g.binary(kir::KOp::Max, s, konstf(0.0));
        s     = g.binary(kir::KOp::Min, s, konstf(1023.0));
        s     = g.unary(kir::KOp::Floor, s);
        return g.cast(s, kir::DType::I32);
    };
    const int ex = expand_ckir(g, quantize(x), sh);
    const int ey = expand_ckir(g, quantize(y), sh);
    const int ez = expand_ckir(g, quantize(z), sh);

    auto      konsti = [&](crd::i64 c) { return g.constant(static_cast<crd::f64>(c), sh, kir::DType::I32); };
    const int mxy    = g.binary(kir::KOp::BitOr, g.binary(kir::KOp::Shl, ex, konsti(2)), g.binary(kir::KOp::Shl, ey, konsti(1)));
    const int morton = g.binary(kir::KOp::BitOr, mxy, ez);

    // Choose centroids that quantize UNAMBIGUOUSLY: c = (q + 0.5)/1024 ⇒ c*1024 = q + 0.5 (exact in f32 and f64) ⇒
    // floor = q regardless of rounding. Removes any FP-boundary flake from the proof.
    crd::u32 qx[kN];
    crd::u32 qy[kN];
    crd::u32 qz[kN];
    crd::f64 xv[kN];
    crd::f64 yv[kN];
    crd::f64 zv[kN];
    for (int i = 0; i < kN; ++i)
    {
        qx[i] = static_cast<crd::u32>((i * 7) % 1024);
        qy[i] = static_cast<crd::u32>((i * 13) % 1024);
        qz[i] = static_cast<crd::u32>((i * 29) % 1024);
        xv[i] = (static_cast<crd::f64>(qx[i]) + 0.5) / 1024.0;
        yv[i] = (static_cast<crd::f64>(qy[i]) + 0.5) / 1024.0;
        zv[i] = (static_cast<crd::f64>(qz[i]) + 0.5) / 1024.0;
    }
    const crd::f64* inputs[] = {xv, yv, zv};
    crd::f64        out[kN];
    kir::eval_cpu(g, inputs, &alloc, morton, out);

    int mism = 0;
    for (int i = 0; i < kN; ++i)
    {
        if (static_cast<crd::u32>(out[i]) != morton_ref(qx[i], qy[i], qz[i])) { ++mism; }
    }
    CHECK(mism == 0);
}

TEST_CASE("v17-i: CKIR scatter-add builds a radix histogram, bit-exact vs the reference on the CPU oracle", "[kir][atomics][histogram]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              hn  = 1024; // keys
    constexpr int              hm  = 256;  // bins (one radix digit = low byte)
    const kir::Shape           shn = kir::make_shape({hn});
    const kir::Shape           shm = kir::make_shape({hm});

    // radix histogram of the low byte: digit = key & 0xFF, then count per bin via atomic scatter-add.
    const int keys  = g.input(shn, kir::DType::I32);
    const int digit = g.binary(kir::KOp::BitAnd, keys, g.constant(255.0, shn, kir::DType::I32));
    const int ones  = g.constant(1.0, shn, kir::DType::I32);
    const int hist  = g.scatter_add(digit, ones, shm);

    crd::f64 kv[hn];
    crd::u32 ref[hm];
    for (int i = 0; i < hm; ++i) { ref[i] = 0; }
    for (int i = 0; i < hn; ++i)
    {
        const crd::u32 k = static_cast<crd::u32>((i * 7 + 13) % 4096);
        kv[i]            = static_cast<crd::f64>(k);
        ref[k & 0xFFU]++;
    }
    const crd::f64* inputs[] = {kv};
    crd::f64        out[hm];
    kir::eval_cpu(g, inputs, &alloc, hist, out);

    int mism = 0;
    for (int i = 0; i < hm; ++i)
    {
        if (static_cast<crd::u32>(out[i]) != ref[i]) { ++mism; }
    }
    CHECK(mism == 0);
}

TEST_CASE("v17-i rung 2b: CKIR ops COMPOSE into a correct radix sort (scan-based stable split, CPU oracle)", "[kir][radix]")
{
    // LSD radix via the classic scan-based STABLE SPLIT (one bit per pass): deterministic (no atomic position
    // allocation). Per pass: biti=(k>>bit)&1; f=1-biti; e=exclusive_scan(f); tf=Σf; pos = biti? tf+i-e : e; k=scatter(pos).
    // This proves the CKIR op set (bit ops + ScanSum + ReduceSum + Broadcast + Iota + Select + Scatter) EXPRESSES a full
    // sort — the exact multi-kernel DAG the scheduler (v17-e) must chain on-GPU. Here each pass is one eval_cpu graph.
    crd::memory::TlsfAllocator alloc(64U << 20U);
    constexpr int              rn   = 512;
    constexpr int              bits = 20;
    crd::u32                   orig[rn];
    for (int i = 0; i < rn; ++i) { orig[i] = static_cast<crd::u32>((i * 2654435761U) & 0xFFFFFU); } // 20-bit keys

    crd::f64 cur[rn];
    for (int i = 0; i < rn; ++i) { cur[i] = static_cast<crd::f64>(orig[i]); }

    for (int bit = 0; bit < bits; ++bit)
    {
        kir::KGraph      g(&alloc);
        const kir::Shape sh      = kir::make_shape({rn});
        const int        k       = g.input(sh, kir::DType::I32);
        const int        shifted = g.binary(kir::KOp::Shr, k, g.constant(static_cast<crd::f64>(bit), sh, kir::DType::I32));
        const int        biti    = g.binary(kir::KOp::BitAnd, shifted, g.constant(1.0, sh, kir::DType::I32));
        const int        f       = g.binary(kir::KOp::Sub, g.constant(1.0, sh, kir::DType::I32), biti); // is-false predicate
        const int        incl    = g.scan(f);                                                          // inclusive prefix
        const int        e       = g.binary(kir::KOp::Sub, incl, f);                                    // exclusive prefix
        const int        tf      = g.broadcast(g.reduce(kir::KOp::ReduceSum, f, 1U), sh);               // total falses
        const int        idx     = g.iota(sh, 0, kir::DType::I32);
        const int        trueB   = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Add, tf, idx), e);        // tf + i - e
        const int        pos     = g.select(biti, trueB, e);                                            // biti ? trueB : e
        const int        sorted  = g.scatter(k, pos, k);                                                // permute keys → pos

        const crd::f64* inputs[] = {cur};
        crd::f64        out[rn];
        kir::eval_cpu(g, inputs, &alloc, sorted, out);
        for (int i = 0; i < rn; ++i) { cur[i] = out[i]; }
    }

    crd::u32 ref[rn];
    for (int i = 0; i < rn; ++i) { ref[i] = orig[i]; }
    std::sort(ref, ref + rn); // sorted VALUES oracle (value equality ⇒ stability irrelevant for the key-only sort)

    int mism = 0;
    for (int i = 0; i < rn; ++i) { if (static_cast<crd::u32>(cur[i]) != ref[i]) { ++mism; } }
    CHECK(mism == 0);
}

TEST_CASE("v17 Phase A: CKIR shader intrinsics (fract/step/clamp/mix) match the reference on the CPU oracle", "[kir][intrinsics]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              in = 128;
    const kir::Shape           sh = kir::make_shape({in});
    const int                  x  = g.input(sh, kir::DType::F64);
    const int                  y  = g.input(sh, kir::DType::F64);
    const int                  z  = g.input(sh, kir::DType::F64);
    // out = clamp( mix( fract(x), y, step(0.5, z) ), 0.1, 0.9 )   — exercises unary/binary/ternary intrinsics
    const int fr = g.unary(kir::KOp::Fract, x);
    const int st = g.binary(kir::KOp::Step, g.constant(0.5, sh, kir::DType::F64), z);
    const int mx = g.ternary(kir::KOp::Mix, fr, y, st);
    const int cl = g.ternary(kir::KOp::Clamp, mx, g.constant(0.1, sh, kir::DType::F64), g.constant(0.9, sh, kir::DType::F64));

    crd::f64 xv[in];
    crd::f64 yv[in];
    crd::f64 zv[in];
    crd::f64 ref[in];
    for (int i = 0; i < in; ++i)
    {
        xv[i]                = (0.3 * static_cast<crd::f64>(i)) - 5.0;
        yv[i]                = 0.05 * static_cast<crd::f64>(i);
        zv[i]                = (0.2 * static_cast<crd::f64>(i % 5)) - 0.3;
        const crd::f64 fr_r  = xv[i] - std::floor(xv[i]);
        const crd::f64 st_r  = zv[i] < 0.5 ? 0.0 : 1.0;
        const crd::f64 mx_r  = (fr_r * (1.0 - st_r)) + (yv[i] * st_r);
        const crd::f64 m2    = mx_r > 0.1 ? mx_r : 0.1;
        ref[i]               = m2 < 0.9 ? m2 : 0.9;
    }
    const crd::f64* inputs[] = {xv, yv, zv};
    crd::f64        out[in];
    kir::eval_cpu(g, inputs, &alloc, cl, out);

    int mism = 0;
    for (int i = 0; i < in; ++i) { if (out[i] != ref[i]) { ++mism; } }
    CHECK(mism == 0);
}

TEST_CASE("v17 Phase A3: CKIR vec3 value type + ops (construct/dot/cross/normalize/length/componentwise) vs the CPU oracle", "[kir][vec]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              vn = 64;
    const kir::Shape           sh = kir::make_shape({vn});
    const int                  x  = g.input(sh, kir::DType::F64);
    const int                  y  = g.input(sh, kir::DType::F64);
    const int                  z  = g.input(sh, kir::DType::F64);
    const int                  v  = g.vec3(x, y, z);
    const int                  w  = g.vec3(z, x, y);
    const int                  sm = g.binary(kir::KOp::Add, v, w); // componentwise vec3 + vec3
    const int                  d  = g.dot(v, w);
    const int                  cr = g.cross(v, w);
    const int                  ln = g.vlength(v);
    const int                  nr = g.normalize(v);
    const int                  w4 = g.input(sh, kir::DType::F64);
    const int                  v4 = g.vec4(x, y, z, w4);   // (x,y,z,w) via concat
    const int                  sw = g.swizzle(v, 1, 2, 0); // v.yzx
    const int                  s2 = g.swizzle(v, 0, 1);    // v.xy

    crd::f64 xv[vn];
    crd::f64 yv[vn];
    crd::f64 zv[vn];
    crd::f64 wv[vn];
    for (int i = 0; i < vn; ++i) { xv[i] = (0.5 * i) - 3.0; yv[i] = (0.2 * i) + 1.0; zv[i] = (-0.3 * i) + 2.0; wv[i] = (0.7 * i) - 1.0; }
    const crd::f64* inp[] = {xv, yv, zv, wv};

    int bad = 0;
    { crd::f64 o[vn]; kir::eval_cpu(g, inp, &alloc, d, o);
      for (int i = 0; i < vn; ++i) { const crd::f64 r = (xv[i] * zv[i]) + (yv[i] * xv[i]) + (zv[i] * yv[i]); if (o[i] != r) { ++bad; } } }
    { crd::f64 o[vn * 3]; kir::eval_cpu(g, inp, &alloc, sm, o);
      for (int i = 0; i < vn; ++i) { if (o[i * 3] != xv[i] + zv[i] || o[i * 3 + 1] != yv[i] + xv[i] || o[i * 3 + 2] != zv[i] + yv[i]) { ++bad; } } }
    { crd::f64 o[vn * 3]; kir::eval_cpu(g, inp, &alloc, cr, o);
      for (int i = 0; i < vn; ++i) {
          const crd::f64 ax = xv[i], ay = yv[i], az = zv[i], bx = zv[i], by = xv[i], bz = yv[i];
          if (o[i * 3] != (ay * bz - az * by) || o[i * 3 + 1] != (az * bx - ax * bz) || o[i * 3 + 2] != (ax * by - ay * bx)) { ++bad; } } }
    { crd::f64 o[vn]; kir::eval_cpu(g, inp, &alloc, ln, o);
      for (int i = 0; i < vn; ++i) { const crd::f64 r = crd::math::sqrt((xv[i] * xv[i]) + (yv[i] * yv[i]) + (zv[i] * zv[i])); if (o[i] != r) { ++bad; } } }
    { crd::f64 o[vn * 3]; kir::eval_cpu(g, inp, &alloc, nr, o);
      for (int i = 0; i < vn; ++i) { const crd::f64 l = crd::math::sqrt((xv[i] * xv[i]) + (yv[i] * yv[i]) + (zv[i] * zv[i]));
          if (o[i * 3] != xv[i] / l || o[i * 3 + 1] != yv[i] / l || o[i * 3 + 2] != zv[i] / l) { ++bad; } } }
    { crd::f64 o[vn * 4]; kir::eval_cpu(g, inp, &alloc, v4, o); // vec4(x,y,z,w)
      for (int i = 0; i < vn; ++i) { if (o[i * 4] != xv[i] || o[i * 4 + 1] != yv[i] || o[i * 4 + 2] != zv[i] || o[i * 4 + 3] != wv[i]) { ++bad; } } }
    { crd::f64 o[vn * 3]; kir::eval_cpu(g, inp, &alloc, sw, o); // v.yzx
      for (int i = 0; i < vn; ++i) { if (o[i * 3] != yv[i] || o[i * 3 + 1] != zv[i] || o[i * 3 + 2] != xv[i]) { ++bad; } } }
    { crd::f64 o[vn * 2]; kir::eval_cpu(g, inp, &alloc, s2, o); // v.xy
      for (int i = 0; i < vn; ++i) { if (o[i * 2] != xv[i] || o[i * 2 + 1] != yv[i]) { ++bad; } } }
    CHECK(bad == 0);
}

TEST_CASE("v17 Phase A3: CKIR mat3 (column-major) — mat*vec, mat*mat, transpose vs the CPU oracle", "[kir][mat]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              mn = 32;
    const kir::Shape           sh = kir::make_shape({mn});
    auto                       k  = [&](crd::f64 val) { return g.constant(val, sh, kir::DType::F64); };
    const crd::f64             mm[9] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 0.5}; // column-major flat: m[col*3+row]
    const int                  c0 = g.vec3(k(mm[0]), k(mm[1]), k(mm[2]));             // column 0
    const int                  c1 = g.vec3(k(mm[3]), k(mm[4]), k(mm[5]));
    const int                  c2 = g.vec3(k(mm[6]), k(mm[7]), k(mm[8]));
    const int                  M  = g.mat3(c0, c1, c2);
    const int                  vx = g.input(sh, kir::DType::F64);
    const int                  vy = g.input(sh, kir::DType::F64);
    const int                  vz = g.input(sh, kir::DType::F64);
    const int                  V  = g.vec3(vx, vy, vz);
    const int                  mv = g.mat_mul_vec(M, V); // vec3
    const int                  mt = g.mat_transpose(M);  // mat3
    const int                  m2 = g.mat_mul(M, M);      // mat3

    crd::f64 vv[3][mn];
    for (int i = 0; i < mn; ++i) { vv[0][i] = (0.3 * i) - 2.0; vv[1][i] = (-0.2 * i) + 1.0; vv[2][i] = (0.1 * i) + 0.5; }
    const crd::f64* inp[] = {vv[0], vv[1], vv[2]};

    int bad = 0;
    { crd::f64 o[mn * 3]; kir::eval_cpu(g, inp, &alloc, mv, o);
      for (int i = 0; i < mn; ++i) { for (int r = 0; r < 3; ++r) { crd::f64 s = 0.0; for (int col = 0; col < 3; ++col) { s += mm[col * 3 + r] * vv[col][i]; } if (o[i * 3 + r] != s) { ++bad; } } } }
    { crd::f64 o[mn * 9]; kir::eval_cpu(g, inp, &alloc, mt, o);
      for (int i = 0; i < mn; ++i) { for (int col = 0; col < 3; ++col) { for (int r = 0; r < 3; ++r) { if (o[i * 9 + col * 3 + r] != mm[r * 3 + col]) { ++bad; } } } } }
    { crd::f64 o[mn * 9]; kir::eval_cpu(g, inp, &alloc, m2, o);
      for (int i = 0; i < mn; ++i) { for (int col = 0; col < 3; ++col) { for (int r = 0; r < 3; ++r) { crd::f64 s = 0.0; for (int kk = 0; kk < 3; ++kk) { s += mm[kk * 3 + r] * mm[col * 3 + kk]; } if (o[i * 9 + col * 3 + r] != s) { ++bad; } } } } }
    CHECK(bad == 0);
}

TEST_CASE("v17 gap-fill: CKIR comparisons (gt/ge/ne) + bit ops (not/count/lsb/msb/extract) vs the CPU oracle", "[kir][bitops]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              bn = 64;
    const kir::Shape           sh = kir::make_shape({bn});
    const int                  a  = g.input(sh, kir::DType::F64);
    const int                  b  = g.input(sh, kir::DType::F64);
    const int                  gt = g.binary(kir::KOp::CmpGt, a, b);
    const int                  ge = g.binary(kir::KOp::CmpGe, a, b);
    const int                  nq = g.binary(kir::KOp::CmpNe, a, b);
    const int                  kk = g.input(sh, kir::DType::I32);
    const int                  bnot = g.unary(kir::KOp::BitNot, kk);
    const int                  bcnt = g.unary(kir::KOp::BitCount, kk);
    const int                  blsb = g.unary(kir::KOp::FindLSB, kk);
    const int                  bmsb = g.unary(kir::KOp::FindMSB, kk);
    const int                  bext = g.ternary(kir::KOp::BitfieldExtract, kk, g.constant(2.0, sh, kir::DType::I32), g.constant(4.0, sh, kir::DType::I32));

    crd::f64 av[bn];
    crd::f64 bv[bn];
    crd::f64 kv[bn];
    for (int i = 0; i < bn; ++i) { av[i] = (0.5 * i) - 10.0; bv[i] = (static_cast<crd::f64>(i % 3) * 3.0) - 5.0; kv[i] = static_cast<crd::f64>((static_cast<crd::u32>(i) * 2654435761U) & 0x3FFFFFFFU); }
    const crd::f64* inp[] = {av, bv, kv};

    int bad = 0;
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, gt, o); for (int i = 0; i < bn; ++i) { if (o[i] != (av[i] > bv[i] ? 1.0 : 0.0)) { ++bad; } } }
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, ge, o); for (int i = 0; i < bn; ++i) { if (o[i] != (av[i] >= bv[i] ? 1.0 : 0.0)) { ++bad; } } }
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, nq, o); for (int i = 0; i < bn; ++i) { if (o[i] != (av[i] != bv[i] ? 1.0 : 0.0)) { ++bad; } } }
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, bnot, o); for (int i = 0; i < bn; ++i) { if (o[i] != static_cast<crd::f64>(~static_cast<crd::i64>(kv[i]))) { ++bad; } } }
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, bcnt, o); for (int i = 0; i < bn; ++i) { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(kv[i])); int c = 0; while (v != 0U) { c += static_cast<int>(v & 1U); v >>= 1U; } if (o[i] != static_cast<crd::f64>(c)) { ++bad; } } }
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, blsb, o); for (int i = 0; i < bn; ++i) { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(kv[i])); crd::f64 r = -1.0; if (v != 0U) { int j = 0; while ((v & 1U) == 0U) { ++j; v >>= 1U; } r = static_cast<crd::f64>(j); } if (o[i] != r) { ++bad; } } }
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, bmsb, o); for (int i = 0; i < bn; ++i) { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(kv[i])); int j = -1; while (v != 0U) { ++j; v >>= 1U; } if (o[i] != static_cast<crd::f64>(j)) { ++bad; } } }
    { crd::f64 o[bn]; kir::eval_cpu(g, inp, &alloc, bext, o); for (int i = 0; i < bn; ++i) { const crd::i64 iv = static_cast<crd::i64>(kv[i]); if (o[i] != static_cast<crd::f64>((iv >> 2) & ((static_cast<crd::i64>(1) << 4) - 1))) { ++bad; } } }
    CHECK(bad == 0);
}

TEST_CASE("v17 gap-fill: CKIR geometric (reflect/refract/faceforward/distance) + relational (any/all) + splat vs the CPU oracle", "[kir][geometric]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              gn = 48;
    const kir::Shape           sh = kir::make_shape({gn});
    const int                  ix = g.input(sh, kir::DType::F64), iy = g.input(sh, kir::DType::F64), iz = g.input(sh, kir::DType::F64);
    const int                  nx = g.input(sh, kir::DType::F64), ny = g.input(sh, kir::DType::F64), nz = g.input(sh, kir::DType::F64);
    const int                  et = g.input(sh, kir::DType::F64);
    const int                  I  = g.vec3(ix, iy, iz);
    const int                  N  = g.normalize(g.vec3(nx, ny, nz));
    const int                  rfl = g.reflect(I, N);
    const int                  rfr = g.refract(I, N, et);
    const int                  ff  = g.faceforward(N, I, N);
    const int                  dd  = g.distance(I, N);
    const int                  sp  = g.splat(et, 3);
    const int                  an  = g.vany(I);
    const int                  al  = g.vall(I);

    crd::f64 iv[3][gn];
    crd::f64 nv[3][gn];
    crd::f64 ev[gn];
    for (int i = 0; i < gn; ++i) { iv[0][i] = (0.4 * i) - 5.0; iv[1][i] = (-0.3 * i) + 2.0; iv[2][i] = (0.2 * i) - 1.0; nv[0][i] = 0.5 + (0.1 * (i % 4)); nv[1][i] = 1.0 - (0.05 * (i % 5)); nv[2][i] = 0.3 + (0.07 * (i % 3)); ev[i] = 0.9; }
    const crd::f64* inp[] = {iv[0], iv[1], iv[2], nv[0], nv[1], nv[2], ev};

    int bad = 0;
    for (int i = 0; i < gn; ++i)
    {
        const crd::f64 len = crd::math::sqrt((nv[0][i] * nv[0][i]) + (nv[1][i] * nv[1][i]) + (nv[2][i] * nv[2][i]));
        const crd::f64 Nn[3] = {nv[0][i] / len, nv[1][i] / len, nv[2][i] / len};
        const crd::f64 Iv[3] = {iv[0][i], iv[1][i], iv[2][i]};
        crd::f64       dp = 0.0;
        for (int k = 0; k < 3; ++k) { dp += Nn[k] * Iv[k]; }

        crd::f64 o[gn * 3];
        kir::eval_cpu(g, inp, &alloc, rfl, o);
        for (int k = 0; k < 3; ++k) { if (o[i * 3 + k] != Iv[k] - 2.0 * dp * Nn[k]) { ++bad; } }
        kir::eval_cpu(g, inp, &alloc, rfr, o);
        { const crd::f64 kk = 1.0 - ev[i] * ev[i] * (1.0 - dp * dp); if (kk < 0.0) { for (int k = 0; k < 3; ++k) { if (o[i * 3 + k] != 0.0) { ++bad; } } } else { const crd::f64 cf = ev[i] * dp + crd::math::sqrt(kk); for (int k = 0; k < 3; ++k) { if (o[i * 3 + k] != ev[i] * Iv[k] - cf * Nn[k]) { ++bad; } } } }
        kir::eval_cpu(g, inp, &alloc, ff, o);
        { const crd::f64 s = dp < 0.0 ? 1.0 : -1.0; for (int k = 0; k < 3; ++k) { if (o[i * 3 + k] != s * Nn[k]) { ++bad; } } }
        kir::eval_cpu(g, inp, &alloc, sp, o);
        for (int k = 0; k < 3; ++k) { if (o[i * 3 + k] != ev[i]) { ++bad; } }
    }
    { crd::f64 o[gn]; kir::eval_cpu(g, inp, &alloc, dd, o); for (int i = 0; i < gn; ++i) { const crd::f64 len = crd::math::sqrt((nv[0][i] * nv[0][i]) + (nv[1][i] * nv[1][i]) + (nv[2][i] * nv[2][i])); const crd::f64 dx = iv[0][i] - nv[0][i] / len, dy = iv[1][i] - nv[1][i] / len, dz = iv[2][i] - nv[2][i] / len; if (o[i] != crd::math::sqrt(dx * dx + dy * dy + dz * dz)) { ++bad; } } }
    { crd::f64 o[gn]; kir::eval_cpu(g, inp, &alloc, an, o); for (int i = 0; i < gn; ++i) { const bool any = iv[0][i] != 0.0 || iv[1][i] != 0.0 || iv[2][i] != 0.0; if (o[i] != (any ? 1.0 : 0.0)) { ++bad; } } }
    { crd::f64 o[gn]; kir::eval_cpu(g, inp, &alloc, al, o); for (int i = 0; i < gn; ++i) { const bool all = iv[0][i] != 0.0 && iv[1][i] != 0.0 && iv[2][i] != 0.0; if (o[i] != (all ? 1.0 : 0.0)) { ++bad; } } }
    CHECK(bad == 0);
}

TEST_CASE("v17 gap-fill: CKIR matrix determinant/inverse/outerProduct vs the CPU oracle", "[kir][matgap]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              mn = 16;
    const kir::Shape           sh = kir::make_shape({mn});
    auto                       k  = [&](crd::f64 v) { return g.constant(v, sh, kir::DType::F64); };
    const crd::f64             mm[9] = {2.0, 0.0, 1.0, 1.0, 3.0, 0.0, 0.0, 1.0, 4.0}; // column-major, det = 25
    const int                  M    = g.mat3(g.vec3(k(mm[0]), k(mm[1]), k(mm[2])), g.vec3(k(mm[3]), k(mm[4]), k(mm[5])), g.vec3(k(mm[6]), k(mm[7]), k(mm[8])));
    const int                  det  = g.determinant(M);
    const int                  inv  = g.mat_inverse(M);
    const int                  prod = g.mat_mul(M, inv); // = I
    const crd::f64             av[3] = {1.0, 2.0, 3.0};
    const crd::f64             bv[3] = {4.0, 5.0, 6.0};
    const int                  op = g.outer_product(g.vec3(k(av[0]), k(av[1]), k(av[2])), g.vec3(k(bv[0]), k(bv[1]), k(bv[2])));

    const crd::f64* inp[1] = {nullptr}; // no Input nodes (all const)
    int             bad    = 0;
    { crd::f64 o[mn]; kir::eval_cpu(g, inp, &alloc, det, o); for (int i = 0; i < mn; ++i) { if (o[i] != 25.0) { ++bad; } } }
    { crd::f64 o[mn * 9]; kir::eval_cpu(g, inp, &alloc, prod, o); for (int i = 0; i < mn; ++i) { for (int col = 0; col < 3; ++col) { for (int r = 0; r < 3; ++r) { const crd::f64 ex = (col == r) ? 1.0 : 0.0; if (std::fabs(o[i * 9 + col * 3 + r] - ex) > 1e-12) { ++bad; } } } } }
    { crd::f64 o[mn * 9]; kir::eval_cpu(g, inp, &alloc, op, o); for (int i = 0; i < mn; ++i) { for (int col = 0; col < 3; ++col) { for (int r = 0; r < 3; ++r) { if (o[i * 9 + col * 3 + r] != av[r] * bv[col]) { ++bad; } } } } }
    CHECK(bad == 0);
}

TEST_CASE("v17 gap-fill: CKIR quaternions (axis-angle/mul/conj/rotate/to-mat3) + slerp vs the CPU oracle", "[kir][quat]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              qn = 16;
    const kir::Shape           sh = kir::make_shape({qn});
    const int                  ax = g.input(sh, kir::DType::F64), ay = g.input(sh, kir::DType::F64), az = g.input(sh, kir::DType::F64);
    const int                  ang = g.input(sh, kir::DType::F64);
    const int                  vx = g.input(sh, kir::DType::F64), vy = g.input(sh, kir::DType::F64), vz = g.input(sh, kir::DType::F64);
    const int                  q   = g.quat_axis_angle(g.normalize(g.vec3(ax, ay, az)), ang);
    const int                  v   = g.vec3(vx, vy, vz);
    const int                  qr  = g.quat_rotate(q, v);                  // rotate v by q
    const int                  mv  = g.mat_mul_vec(g.quat_to_mat3(q), v);  // rotate v by q's matrix — must match qr
    const int                  qqc = g.quat_mul(q, g.quat_conj(q));        // = identity (0,0,0,1)
    const int                  sl  = g.slerp(q, q, g.constant(0.5, sh, kir::DType::F64)); // = q

    crd::f64 axv[3][qn];
    crd::f64 angv[qn];
    crd::f64 vv[3][qn];
    for (int i = 0; i < qn; ++i) { axv[0][i] = 0.3 + 0.1 * i; axv[1][i] = 1.0 - 0.05 * i; axv[2][i] = 0.2 + 0.07 * i; angv[i] = 0.2 + 0.3 * i; vv[0][i] = (0.5 * i) - 2.0; vv[1][i] = 1.0 + 0.2 * i; vv[2][i] = (-0.3 * i) + 1.0; }
    const crd::f64* inp[] = {axv[0], axv[1], axv[2], angv, vv[0], vv[1], vv[2]};

    int bad = 0;
    { crd::f64 a[qn * 3]; crd::f64 b[qn * 3]; kir::eval_cpu(g, inp, &alloc, qr, a); kir::eval_cpu(g, inp, &alloc, mv, b); for (int i = 0; i < qn * 3; ++i) { if (std::fabs(a[i] - b[i]) > 1e-12) { ++bad; } } }        // rotate ≡ mat·v
    { crd::f64 o[qn * 4]; kir::eval_cpu(g, inp, &alloc, qqc, o); for (int i = 0; i < qn; ++i) { if (std::fabs(o[i * 4]) > 1e-12 || std::fabs(o[i * 4 + 1]) > 1e-12 || std::fabs(o[i * 4 + 2]) > 1e-12 || std::fabs(o[i * 4 + 3] - 1.0) > 1e-12) { ++bad; } } } // q·conj ≡ identity
    { crd::f64 a[qn * 4]; crd::f64 b[qn * 4]; kir::eval_cpu(g, inp, &alloc, sl, a); kir::eval_cpu(g, inp, &alloc, q, b); for (int i = 0; i < qn * 4; ++i) { if (std::fabs(a[i] - b[i]) > 1e-12) { ++bad; } } }          // slerp(q,q) ≡ q
    CHECK(bad == 0);
}

TEST_CASE("v17 gap-fill: CKIR bitfieldInsert/reverse + ldexp + float<->int bits + modf vs the CPU oracle", "[kir][minorgap]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              n  = 32;
    const kir::Shape           sh = kir::make_shape({n});
    const int                  kk = g.input(sh, kir::DType::I32);
    const int                  in = g.input(sh, kir::DType::I32);
    const int                  fx = g.input(sh, kir::DType::F32);
    const int                  rev = g.bit_reverse(kk);
    const int                  bi  = g.bitfield_insert(kk, in, g.constant(4.0, sh, kir::DType::I32), g.constant(8.0, sh, kir::DType::I32));
    const int                  rt  = g.int_bits_to_float(g.float_bits_to_int(fx)); // roundtrip = fx
    const int                  lx  = g.ldexp(fx, g.constant(3.0, sh, kir::DType::I32));
    const int                  md  = g.modf(fx);

    crd::f64 kv[n];
    crd::f64 iv[n];
    crd::f64 fv[n];
    for (int i = 0; i < n; ++i) { kv[i] = static_cast<crd::f64>((static_cast<crd::u32>(i) * 2654435761U) & 0x3FFFFFFFU); iv[i] = static_cast<crd::f64>((static_cast<crd::u32>(i) * 40503U) & 0xFFU); fv[i] = static_cast<crd::f64>(static_cast<float>((0.5 * i) - 5.0)); }
    const crd::f64* inp[] = {kv, iv, fv};

    int bad = 0;
    { crd::f64 o[n]; kir::eval_cpu(g, inp, &alloc, rev, o); for (int i = 0; i < n; ++i) { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(kv[i])); crd::u32 r = 0U; for (int b = 0; b < 32; ++b) { r = (r << 1U) | (v & 1U); v >>= 1U; } if (o[i] != static_cast<crd::f64>(r)) { ++bad; } } }
    { crd::f64 o[n]; kir::eval_cpu(g, inp, &alloc, bi, o); for (int i = 0; i < n; ++i) { const crd::i64 base = static_cast<crd::i64>(kv[i]); const crd::i64 insv = static_cast<crd::i64>(iv[i]); const crd::i64 mask = ((static_cast<crd::i64>(1) << 8) - 1) << 4; if (o[i] != static_cast<crd::f64>((base & ~mask) | ((insv << 4) & mask))) { ++bad; } } }
    { crd::f64 o[n]; kir::eval_cpu(g, inp, &alloc, rt, o); for (int i = 0; i < n; ++i) { if (o[i] != fv[i]) { ++bad; } } }
    { crd::f64 o[n]; kir::eval_cpu(g, inp, &alloc, lx, o); for (int i = 0; i < n; ++i) { if (o[i] != fv[i] * 8.0) { ++bad; } } }
    { crd::f64 o[n * 2]; kir::eval_cpu(g, inp, &alloc, md, o); for (int i = 0; i < n; ++i) { const crd::f64 ip = crd::math::trunc(fv[i]); if (o[i * 2] != ip || o[i * 2 + 1] != fv[i] - ip) { ++bad; } } }
    CHECK(bad == 0);
}

TEST_CASE("v17 A4: CKIR unroll_for (fixed-count loop) accumulates correctly on the CPU oracle", "[kir][controlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              cn = 64;
    const kir::Shape           sh = kir::make_shape({cn});
    const int                  x  = g.input(sh, kir::DType::F64);
    const int                  y  = g.input(sh, kir::DType::F64);
    // acc = x; for i in [0,8): acc = acc + i*y   →   x + y*(0+1+...+7)
    const int r = g.unroll_for(8, x, [&](int i, int acc) { return g.binary(kir::KOp::Add, acc, g.binary(kir::KOp::Mul, g.constant(static_cast<crd::f64>(i), sh, kir::DType::F64), y)); });

    crd::f64 xv[cn];
    crd::f64 yv[cn];
    for (int i = 0; i < cn; ++i) { xv[i] = (0.3 * i) - 2.0; yv[i] = 0.1 + (0.02 * i); }
    const crd::f64* inp[] = {xv, yv};
    crd::f64        out[cn];
    kir::eval_cpu(g, inp, &alloc, r, out);

    int mism = 0;
    for (int i = 0; i < cn; ++i)
    {
        crd::f64 acc = xv[i];
        for (int it = 0; it < 8; ++it) { acc = acc + (static_cast<crd::f64>(it) * yv[i]); }
        if (out[i] != acc) { ++mism; }
    }
    CHECK(mism == 0);
}

TEST_CASE("v17 A4 tier-2: CKIR dynamic for_loop (divergent per-element count) on the CPU oracle", "[kir][controlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              cn  = 64;
    const kir::Shape           sh  = kir::make_shape({cn});
    const int                  x   = g.input(sh, kir::DType::F64);
    const int                  y   = g.input(sh, kir::DType::F64);
    const int                  cnt = g.input(sh, kir::DType::F64); // per-element (divergent) trip count
    // acc = x; for it in [0, cnt): acc = acc + y   →   x + cnt*y   (divergent: each element loops its own cnt)
    const int r = g.for_loop(cnt, x, [&](int /*idx*/, int acc) { return g.binary(kir::KOp::Add, acc, y); });

    crd::f64 xv[cn];
    crd::f64 yv[cn];
    crd::f64 cv[cn];
    for (int i = 0; i < cn; ++i) { xv[i] = (0.3 * i) - 2.0; yv[i] = 0.1 + (0.02 * i); cv[i] = static_cast<crd::f64>(i % 9); }
    const crd::f64* inp[] = {xv, yv, cv};
    crd::f64        out[cn];
    kir::eval_cpu(g, inp, &alloc, r, out);

    int mism = 0;
    for (int i = 0; i < cn; ++i)
    {
        crd::f64  acc = xv[i];
        const int c   = static_cast<int>(cv[i]);
        for (int it = 0; it < c; ++it) { acc = acc + yv[i]; }
        if (out[i] != acc) { ++mism; }
    }
    CHECK(mism == 0);
}

TEST_CASE("v17 A4 tier-2: CKIR bounded while_loop + switch_case (compositions) on the CPU oracle", "[kir][controlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    constexpr int              cn  = 64;
    const kir::Shape           sh  = kir::make_shape({cn});
    int                        bad = 0;

    { // bounded while: acc = 0; while (acc < thr) acc += step  (max 100 iters, freeze on !cond)
        kir::KGraph g(&alloc);
        const int   thr  = g.input(sh, kir::DType::F64);
        const int   step = g.input(sh, kir::DType::F64);
        const int   zero = g.constant(0.0, sh, kir::DType::F64);
        const int   r    = g.while_loop(100, zero, [&](int acc) { return g.binary(kir::KOp::CmpGt, thr, acc); }, [&](int /*idx*/, int acc) { return g.binary(kir::KOp::Add, acc, step); });
        crd::f64    tv[cn];
        crd::f64    sv[cn];
        for (int i = 0; i < cn; ++i) { tv[i] = 5.0 + (0.1 * i); sv[i] = 0.7 + (0.01 * i); }
        const crd::f64* inp[] = {tv, sv};
        crd::f64        out[cn];
        kir::eval_cpu(g, inp, &alloc, r, out);
        for (int i = 0; i < cn; ++i) { crd::f64 acc = 0.0; for (int it = 0; it < 100; ++it) { if (acc < tv[i]) { acc = acc + sv[i]; } } if (out[i] != acc) { ++bad; } }
    }
    { // switch: sel==0 ? a : sel==1 ? b : c
        kir::KGraph g(&alloc);
        const int   sel = g.input(sh, kir::DType::F64);
        const int   a   = g.input(sh, kir::DType::F64);
        const int   b   = g.input(sh, kir::DType::F64);
        const int   c   = g.input(sh, kir::DType::F64);
        const int   r   = g.switch_case(sel, g.constant(0.0, sh, kir::DType::F64), a, g.switch_case(sel, g.constant(1.0, sh, kir::DType::F64), b, c));
        crd::f64    se[cn];
        crd::f64    av[cn];
        crd::f64    bv[cn];
        crd::f64    cv[cn];
        for (int i = 0; i < cn; ++i) { se[i] = static_cast<crd::f64>(i % 3); av[i] = 1.0 + i; bv[i] = 100.0 + i; cv[i] = -50.0 - i; }
        const crd::f64* inp[] = {se, av, bv, cv};
        crd::f64        out[cn];
        kir::eval_cpu(g, inp, &alloc, r, out);
        for (int i = 0; i < cn; ++i) { const crd::f64 ref = (se[i] == 0.0) ? av[i] : ((se[i] == 1.0) ? bv[i] : cv[i]); if (out[i] != ref) { ++bad; } }
    }
    CHECK(bad == 0);
}

TEST_CASE("v17 gap-fill: CKIR translate/scale transforms compose correctly (CPU oracle)", "[kir][transform]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              n  = 4;
    const kir::Shape           sh = kir::make_shape({n});
    auto                       k  = [&](crd::f64 v) { return g.constant(v, sh, kir::DType::F64); };
    const int                  tr = g.translate(k(2.0), k(3.0), k(4.0));
    const int                  sc = g.scale(k(5.0), k(6.0), k(7.0));
    const crd::f64* inp[1] = {nullptr};
    int             bad    = 0;
    { crd::f64 o[n * 16]; kir::eval_cpu(g, inp, &alloc, tr, o); const crd::f64 ex[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1}; for (int i = 0; i < n; ++i) { for (int j = 0; j < 16; ++j) { if (o[i * 16 + j] != ex[j]) { ++bad; } } } }
    { crd::f64 o[n * 16]; kir::eval_cpu(g, inp, &alloc, sc, o); const crd::f64 ex[16] = {5, 0, 0, 0, 0, 6, 0, 0, 0, 0, 7, 0, 0, 0, 0, 1}; for (int i = 0; i < n; ++i) { for (int j = 0; j < 16; ++j) { if (o[i * 16 + j] != ex[j]) { ++bad; } } } }
    CHECK(bad == 0);
}
