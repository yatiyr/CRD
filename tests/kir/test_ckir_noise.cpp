// test_ckir_noise.cpp — D-007 B6-b: the MaterialX SOURCE noise nodes (crd::kir::nodes::noise) proven BIT-EXACT against a
// faithful C++ transcription of MaterialX's mx_noise.glsl (the OSL oslnoise subset) on CKIR's CPU oracle. The graph runs in
// F64 (its constants derive dtype from the operands), so this isolates the OPERATION STRUCTURE — most importantly that the
// 32-bit Bob-Jenkins hash reproduces exactly on the f64/i64 IR (split-rotl + wraparound masks). The reference below is a
// direct line-by-line port of mx_noise.glsl: the hash in uint32_t (exact), the gradient/fade/lerp/scale in double.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_noise.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>   // std::floor for the reference floorfrac
#include <cstdint> // uint32_t hash

namespace kir = crd::kir;
namespace nz  = crd::kir::nodes::noise;

namespace
{
constexpr int kN = 24;

// ── reference: a faithful port of mx_noise.glsl ──────────────────────────────────────────────────────────────────────
crd::u32 ref_rotl32(crd::u32 x, int k) { return (x << k) | (x >> (32 - k)); }
void     ref_bjmix(crd::u32& a, crd::u32& b, crd::u32& c)
{
    a -= c; a ^= ref_rotl32(c, 4);  c += b;
    b -= a; b ^= ref_rotl32(a, 6);  a += c;
    c -= b; c ^= ref_rotl32(b, 8);  b += a;
    a -= c; a ^= ref_rotl32(c, 16); c += b;
    b -= a; b ^= ref_rotl32(a, 19); a += c;
    c -= b; c ^= ref_rotl32(b, 4);  b += a;
}
crd::u32 ref_bjfinal(crd::u32 a, crd::u32 b, crd::u32 c)
{
    c ^= b; c -= ref_rotl32(b, 14);
    a ^= c; a -= ref_rotl32(c, 11);
    b ^= a; b -= ref_rotl32(a, 25);
    c ^= b; c -= ref_rotl32(b, 16);
    a ^= c; a -= ref_rotl32(c, 4);
    b ^= a; b -= ref_rotl32(a, 14);
    c ^= b; c -= ref_rotl32(b, 24);
    return c;
}
crd::u32 ref_hash2(int x, int y)
{
    const crd::u32 seed = 0xdeadbeefU + (2U << 2U) + 13U;
    crd::u32       a    = seed + static_cast<crd::u32>(x);
    crd::u32       b    = seed + static_cast<crd::u32>(y);
    return ref_bjfinal(a, b, seed);
}
crd::u32 ref_hash3(int x, int y, int z)
{
    const crd::u32 seed = 0xdeadbeefU + (3U << 2U) + 13U;
    crd::u32       a    = seed + static_cast<crd::u32>(x);
    crd::u32       b    = seed + static_cast<crd::u32>(y);
    crd::u32       c    = seed + static_cast<crd::u32>(z);
    return ref_bjfinal(a, b, c);
}
double ref_fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
double ref_negif(double v, bool b) { return b ? -v : v; }
double ref_bilerp(double v0, double v1, double v2, double v3, double s, double t)
{
    const double s1 = 1.0 - s;
    return (1.0 - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}
double ref_trilerp(const double v[8], double s, double t, double r)
{
    const double s1 = 1.0 - s;
    const double t1 = 1.0 - t;
    const double r1 = 1.0 - r;
    return r1 * (t1 * (v[0] * s1 + v[1] * s) + t * (v[2] * s1 + v[3] * s)) + r * (t1 * (v[4] * s1 + v[5] * s) + t * (v[6] * s1 + v[7] * s));
}
double ref_grad2(crd::u32 hash, double x, double y)
{
    const crd::u32 h = hash & 7U;
    const double   u = h < 4U ? x : y;
    const double   v = 2.0 * (h < 4U ? y : x);
    return ref_negif(u, (h & 1U) != 0U) + ref_negif(v, (h & 2U) != 0U);
}
double ref_grad3(crd::u32 hash, double x, double y, double z)
{
    const crd::u32 h  = hash & 15U;
    const double   u  = h < 8U ? x : y;
    const bool     hz = (h == 12U) || (h == 14U);
    double         v  = z;
    if (h < 4U) { v = y; }
    else if (hz) { v = x; }
    return ref_negif(u, (h & 1U) != 0U) + ref_negif(v, (h & 2U) != 0U);
}
double ref_perlin2(double px, double py)
{
    const int    x = static_cast<int>(std::floor(px));
    const int    y = static_cast<int>(std::floor(py));
    const double fx = px - std::floor(px);
    const double fy = py - std::floor(py);
    const double u  = ref_fade(fx);
    const double v  = ref_fade(fy);
    const double r  = ref_bilerp(ref_grad2(ref_hash2(x, y), fx, fy), ref_grad2(ref_hash2(x + 1, y), fx - 1.0, fy), ref_grad2(ref_hash2(x, y + 1), fx, fy - 1.0), ref_grad2(ref_hash2(x + 1, y + 1), fx - 1.0, fy - 1.0), u, v);
    return 0.6616 * r;
}
double ref_perlin3(double px, double py, double pz)
{
    const int    x = static_cast<int>(std::floor(px));
    const int    y = static_cast<int>(std::floor(py));
    const int    z = static_cast<int>(std::floor(pz));
    const double fx = px - std::floor(px);
    const double fy = py - std::floor(py);
    const double fz = pz - std::floor(pz);
    const double c[8] = {
        ref_grad3(ref_hash3(x, y, z), fx, fy, fz),           ref_grad3(ref_hash3(x + 1, y, z), fx - 1.0, fy, fz),
        ref_grad3(ref_hash3(x, y + 1, z), fx, fy - 1.0, fz), ref_grad3(ref_hash3(x + 1, y + 1, z), fx - 1.0, fy - 1.0, fz),
        ref_grad3(ref_hash3(x, y, z + 1), fx, fy, fz - 1.0), ref_grad3(ref_hash3(x + 1, y, z + 1), fx - 1.0, fy, fz - 1.0),
        ref_grad3(ref_hash3(x, y + 1, z + 1), fx, fy - 1.0, fz - 1.0), ref_grad3(ref_hash3(x + 1, y + 1, z + 1), fx - 1.0, fy - 1.0, fz - 1.0),
    };
    return 0.9820 * ref_trilerp(c, ref_fade(fx), ref_fade(fy), ref_fade(fz));
}
double ref_cell2(double px, double py) { return static_cast<double>(ref_hash2(static_cast<int>(std::floor(px)), static_cast<int>(std::floor(py)))) / static_cast<double>(0xffffffffU); }
double ref_cell3(double px, double py, double pz) { return static_cast<double>(ref_hash3(static_cast<int>(std::floor(px)), static_cast<int>(std::floor(py)), static_cast<int>(std::floor(pz)))) / static_cast<double>(0xffffffffU); }
double ref_fractal2(double px, double py, int oct, double lac, double dim)
{
    double acc = 0.0;
    double fx = px;
    double fy = py;
    double amp = 1.0;
    for (int o = 0; o < oct; ++o) { acc += amp * ref_perlin2(fx, fy); fx *= lac; fy *= lac; amp *= dim; }
    return acc;
}
// ── worley reference (mx_worley_*) ──
double   ref_bits01(crd::u32 b) { return static_cast<double>(b) / static_cast<double>(0xffffffffU); }
int      ref_ifloor(double p) { return static_cast<int>(std::floor(p)); }
double   ref_dabs(double x) { return x < 0.0 ? -x : x; }
void     ref_worley_off2(int ax, int ay, double jitter, double out[2])
{
    out[0] = (ref_bits01(ref_hash3(ax, ay, 0)) - 0.5) * jitter + 0.5;
    out[1] = (ref_bits01(ref_hash3(ax, ay, 1)) - 0.5) * jitter + 0.5;
}
double ref_worley2(double px, double py, double jitter, int style, int metric)
{
    const int x0 = ref_ifloor(px);
    const int y0 = ref_ifloor(py);
    const double lx = px - std::floor(px);
    const double ly = py - std::floor(py);
    double best = 1e6;
    double mp[2] = {0.0, 0.0};
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            double off[2];
            ref_worley_off2(x0 + x, y0 + y, jitter, off);
            const double dx = (static_cast<double>(x) + off[0]) - lx;
            const double dy = (static_cast<double>(y) + off[1]) - ly;
            double dist = dx * dx + dy * dy;
            if (metric == 2) { dist = ref_dabs(dx) + ref_dabs(dy); }
            else if (metric == 3) { const double axd = ref_dabs(dx); const double ayd = ref_dabs(dy); dist = axd > ayd ? axd : ayd; }
            if (dist < best) { best = dist; mp[0] = dx; mp[1] = dy; }
        }
    }
    if (style == 1) { return ref_cell2(mp[0] + px, mp[1] + py); }
    if (metric == 0) { return std::sqrt(best); }
    return best;
}
void ref_cell_vec3_from3(int ix, int iy, int iz, double out[3])
{
    const crd::u32 seed = 0xdeadbeefU + (4U << 2U) + 13U;
    crd::u32       a    = seed + static_cast<crd::u32>(ix);
    crd::u32       b    = seed + static_cast<crd::u32>(iy);
    crd::u32       c    = seed + static_cast<crd::u32>(iz);
    ref_bjmix(a, b, c);
    out[0] = ref_bits01(ref_bjfinal(a, b, c));
    out[1] = ref_bits01(ref_bjfinal(a + 1U, b, c));
    out[2] = ref_bits01(ref_bjfinal(a + 2U, b, c));
}
double ref_worley3(double px, double py, double pz, double jitter, int style, int metric)
{
    const int x0 = ref_ifloor(px);
    const int y0 = ref_ifloor(py);
    const int z0 = ref_ifloor(pz);
    const double lx = px - std::floor(px);
    const double ly = py - std::floor(py);
    const double lz = pz - std::floor(pz);
    double best = 1e6;
    double mp[3] = {0.0, 0.0, 0.0};
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int z = -1; z <= 1; ++z)
            {
                double cn[3];
                ref_cell_vec3_from3(x0 + x, y0 + y, z0 + z, cn);
                const double dx = (static_cast<double>(x) + ((cn[0] - 0.5) * jitter + 0.5)) - lx;
                const double dy = (static_cast<double>(y) + ((cn[1] - 0.5) * jitter + 0.5)) - ly;
                const double dz = (static_cast<double>(z) + ((cn[2] - 0.5) * jitter + 0.5)) - lz;
                double dist = dx * dx + dy * dy + dz * dz;
                if (metric == 2) { dist = ref_dabs(dx) + ref_dabs(dy) + ref_dabs(dz); }
                else if (metric == 3) { const double m0 = ref_dabs(dx) > ref_dabs(dy) ? ref_dabs(dx) : ref_dabs(dy); dist = m0 > ref_dabs(dz) ? m0 : ref_dabs(dz); }
                if (dist < best) { best = dist; mp[0] = dx; mp[1] = dy; mp[2] = dz; }
            }
        }
    }
    if (style == 1) { return ref_cell3(mp[0] + px, mp[1] + py, mp[2] + pz); }
    if (metric == 0) { return std::sqrt(best); }
    return best;
}
} // namespace

TEST_CASE("B6-b: noise hash + perlin/cell/fractal bit-exact vs MaterialX mx_noise on the CPU oracle", "[kir][nodes][noise]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int px = g.input(sh, kir::DType::F64);
    const int py = g.input(sh, kir::DType::F64);
    const int pz = g.input(sh, kir::DType::F64);

    crd::f64 xv[kN];
    crd::f64 yv[kN];
    crd::f64 zv[kN];
    for (int i = 0; i < kN; ++i)
    {
        xv[i] = (0.37 * i) - 4.0; // spans negative & positive → exercises int->uint on the hash coords
        yv[i] = (0.53 * i) - 2.5;
        zv[i] = (0.29 * i) - 1.5;
    }
    const crd::f64* inp[] = {xv, yv, zv};

    int        bad = 0;
    const auto chk = [&](int node, auto ref) { crd::f64 o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };

    chk(nz::perlin2(g, px, py), [&](int i) { return ref_perlin2(xv[i], yv[i]); });
    chk(nz::perlin3(g, px, py, pz), [&](int i) { return ref_perlin3(xv[i], yv[i], zv[i]); });
    chk(nz::cell2(g, px, py), [&](int i) { return ref_cell2(xv[i], yv[i]); });
    chk(nz::cell3(g, px, py, pz), [&](int i) { return ref_cell3(xv[i], yv[i], zv[i]); });
    chk(nz::fractal2(g, px, py, 3, 2.0, 0.5), [&](int i) { return ref_fractal2(xv[i], yv[i], 3, 2.0, 0.5); });

    CHECK(bad == 0);
}

TEST_CASE("B6-b: worley (cellular) noise bit-exact vs MaterialX mx_worley on the CPU oracle", "[kir][nodes][noise][worley]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int px = g.input(sh, kir::DType::F64);
    const int py = g.input(sh, kir::DType::F64);
    const int pz = g.input(sh, kir::DType::F64);

    crd::f64 xv[kN];
    crd::f64 yv[kN];
    crd::f64 zv[kN];
    for (int i = 0; i < kN; ++i)
    {
        xv[i] = (0.41 * i) - 3.0;
        yv[i] = (0.33 * i) - 2.0;
        zv[i] = (0.27 * i) - 1.0;
    }
    const crd::f64* inp[] = {xv, yv, zv};

    int        bad = 0;
    const auto chk = [&](int node, auto ref) { crd::f64 o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };

    // 2D: every metric (0=euclid,1=dist²,2=manhattan,3=chebyshev) + both styles, jitter 1.0.
    chk(nz::worley2(g, px, py, 1.0, 0, 0), [&](int i) { return ref_worley2(xv[i], yv[i], 1.0, 0, 0); });
    chk(nz::worley2(g, px, py, 1.0, 0, 1), [&](int i) { return ref_worley2(xv[i], yv[i], 1.0, 0, 1); });
    chk(nz::worley2(g, px, py, 1.0, 0, 2), [&](int i) { return ref_worley2(xv[i], yv[i], 1.0, 0, 2); });
    chk(nz::worley2(g, px, py, 1.0, 0, 3), [&](int i) { return ref_worley2(xv[i], yv[i], 1.0, 0, 3); });
    chk(nz::worley2(g, px, py, 0.75, 1, 0), [&](int i) { return ref_worley2(xv[i], yv[i], 0.75, 1, 0); }); // style 1 (cell value)
    // 3D: exercises the 4-arg hash (bjmix). Euclidean + manhattan.
    chk(nz::worley3(g, px, py, pz, 1.0, 0, 0), [&](int i) { return ref_worley3(xv[i], yv[i], zv[i], 1.0, 0, 0); });
    chk(nz::worley3(g, px, py, pz, 1.0, 0, 2), [&](int i) { return ref_worley3(xv[i], yv[i], zv[i], 1.0, 0, 2); });

    CHECK(bad == 0);
}
