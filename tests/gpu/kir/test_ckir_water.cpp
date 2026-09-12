// test_ckir_water.cpp — D-007 B16-a-4: the CKIR water-shading library (crd::kir::water) proven BIT-EXACT against a line-by-
// line f64 reference on the CPU oracle (the B8 lighting methodology). `water_shade` integrates the air-water Schlick Fresnel,
// a GGX sun highlight, Beer-Lambert depth absorption, subsurface backscatter, and the foam overlay — matching it end-to-end
// validates every term. The graph runs in F64 (constants derive dtype from operands), isolating the operation structure.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_lighting.hpp>
#include <crd/kir/ckir_water.hpp>

#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;
namespace wt  = crd::kir::water;

namespace
{
constexpr int    kN  = 20;
constexpr double kPi = crd::kir::lighting::kPi;

// the constant scene colours — MUST match the graph literals below.
constexpr double kSun[3]   = {5.0, 4.8, 4.2};
constexpr double kSky[3]   = {0.3, 0.5, 0.8};
constexpr double kScene[3] = {0.1, 0.15, 0.12};
constexpr double kDeep[3]  = {0.0, 0.08, 0.12};
constexpr double kExt[3]   = {0.45, 0.09, 0.06};
constexpr double kFoamC[3] = {0.9, 0.92, 0.95};

double rsat(double x) { const double lo = x > 0.0 ? x : 0.0; return lo < 1.0 ? lo : 1.0; }
double rdot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// line-by-line f64 port of water_shade for channel c.
double ref_water(const double n[3], const double v[3], const double l[3], double depth, double wave, double foam, double rough, int c)
{
    const double dnv = rdot(n, v);
    const double nov = dnv > 1e-4 ? dnv : 1e-4;
    const double nol = rsat(rdot(n, l));
    const double hraw[3] = {v[0] + l[0], v[1] + l[1], v[2] + l[2]};
    const double hlen    = crd::math::sqrt(rdot(hraw, hraw));
    const double h[3]    = {hraw[0] / hlen, hraw[1] / hlen, hraw[2] / hlen};
    const double noh     = rsat(rdot(n, h));

    const double t  = 1.0 - nov;
    const double p5 = (t * t) * (t * t) * t;
    const double fr = 0.02 + (1.0 - 0.02) * p5;

    const double alpha = rough * rough;
    const double a     = noh * alpha;
    const double denom = (1.0 - noh * noh) + a * a;
    const double kraw  = alpha / denom;
    const double kk    = kraw < 453.5 ? kraw : 453.5;
    const double dterm = kk * (kk * (1.0 / kPi));

    const double sunf  = dterm * (fr * nol);
    const double reflc = kSky[c] + kSun[c] * sunf;

    const double ac   = crd::math::exp(kExt[c] * (-depth));
    const double refr = kScene[c] * ac + kDeep[c] * (1.0 - ac);

    const double negl[3] = {-l[0], -l[1], -l[2]};
    const double vdl     = rsat(rdot(v, negl));
    const double vdl3    = (vdl * vdl) * vdl;
    const double sss     = kDeep[c] * (rsat(wave) * vdl3);

    const double water   = reflc * fr + (refr + sss) * (1.0 - fr);
    const double foamlit = kFoamC[c] * (nol + 0.2);
    return water * (1.0 - foam) + foamlit * foam;
}

constexpr double kSun2[3]  = {5.0, 4.8, 4.2};
constexpr double kHoriz[3] = {0.7, 0.8, 0.95};
constexpr double kZen[3]   = {0.15, 0.35, 0.75};
constexpr double kSkySun[3] = {8.0, 7.5, 6.0};

// line-by-line f64 port of ocean_sun_glitter.
double ref_glitter(const double n[3], const double v[3], const double l[3], double var, int c)
{
    const double hraw[3] = {v[0] + l[0], v[1] + l[1], v[2] + l[2]};
    const double hlen    = crd::math::sqrt(rdot(hraw, hraw));
    const double h[3]    = {hraw[0] / hlen, hraw[1] / hlen, hraw[2] / hlen};
    const double noh     = rsat(rdot(n, h));
    const double dnv     = rdot(n, v);
    const double nov     = dnv > 1e-4 ? dnv : 1e-4;
    const double nol     = rsat(rdot(n, l));
    const double voh     = rsat(rdot(v, h));
    const double sq2v    = crd::math::sqrt(2.0 * var);
    const double alpha   = sq2v > 0.02 ? sq2v : 0.02;

    const double a     = noh * alpha;
    const double denom = (1.0 - noh * noh) + a * a;
    const double kraw  = alpha / denom;
    const double kk    = kraw < 453.5 ? kraw : 453.5;
    const double dterm = kk * (kk * (1.0 / kPi));

    const double a2 = alpha * alpha;
    const double lv = nol * crd::math::sqrt((nov - a2 * nov) * nov + a2);
    const double ll = nov * crd::math::sqrt((nol - a2 * nol) * nol + a2);
    const double lvll = lv + ll;
    const double vterm = 0.5 / (lvll > 0.0000077 ? lvll : 0.0000077);

    const double t   = 1.0 - voh;
    const double p5  = (t * t) * (t * t) * t;
    const double fr  = 0.02 + (1.0 - 0.02) * p5;
    const double spec = (dterm * vterm) * (fr * nol);
    return kSun2[c] * spec;
}

// line-by-line f64 port of sky_color.
double ref_sky(const double dir[3], const double sun[3], int c)
{
    const double ty   = rsat(dir[1]);
    const double grad = kHoriz[c] * (1.0 - ty) + kZen[c] * ty;
    const double dsun = rdot(dir, sun);
    const double sd   = dsun > 0.0 ? dsun : 0.0;
    const double disk = crd::math::exp((sd - 1.0) * 120.0);
    const double halo = crd::math::exp((sd - 1.0) * 8.0);
    const double glow = disk + halo * 0.15;
    return grad + kSkySun[c] * glow;
}
} // namespace

TEST_CASE("B16-a-4: water_shade (Fresnel dielectric + Beer + SSS + foam) bit-exact vs f64 reference", "[kir][water]")
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
    const int lx = g.input(sh, kir::DType::F64);
    const int ly = g.input(sh, kir::DType::F64);
    const int lz = g.input(sh, kir::DType::F64);
    const int dp = g.input(sh, kir::DType::F64);
    const int wv = g.input(sh, kir::DType::F64);
    const int fm = g.input(sh, kir::DType::F64);
    const int rg = g.input(sh, kir::DType::F64);

    double nrm[3][kN];
    double vw[3][kN];
    double lw[3][kN];
    double dpv[kN];
    double wvv[kN];
    double fmv[kN];
    double rgv[kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        nrm[0][i] = 0.08 * i - 0.7; nrm[1][i] = 1.0;              nrm[2][i] = 0.3 + 0.02 * i;
        vw[0][i]  = 0.2;            vw[1][i]  = 0.9 - 0.03 * i;   vw[2][i]  = 0.6;
        lw[0][i]  = 0.5 - 0.05 * i; lw[1][i]  = 0.7;              lw[2][i]  = 0.2 + 0.03 * i;
        unit(nrm[0][i], nrm[1][i], nrm[2][i]);
        unit(vw[0][i], vw[1][i], vw[2][i]);
        unit(lw[0][i], lw[1][i], lw[2][i]);
        dpv[i] = 0.2 + 0.4 * i;      // depth 0.2..8 m
        wvv[i] = -0.5 + 0.09 * i;    // wave height (some negative ⇒ SSS clamps to 0)
        fmv[i] = 0.05 * i;           // foam 0..0.95
        rgv[i] = 0.02 + 0.03 * i;    // roughness 0.02..0.6
    }
    const double* inp[] = {nrm[0], nrm[1], nrm[2], vw[0], vw[1], vw[2], lw[0], lw[1], lw[2], dpv, wvv, fmv, rgv};

    const auto kc  = [&](double x) { return g.constant(x, sh, kir::DType::F64); };
    const int  nn  = g.vec3(nx, ny, nz);
    const int  vv  = g.vec3(vx, vy, vz);
    const int  ll  = g.vec3(lx, ly, lz);
    const int  sun = g.vec3(kc(kSun[0]), kc(kSun[1]), kc(kSun[2]));
    const int  sky = g.vec3(kc(kSky[0]), kc(kSky[1]), kc(kSky[2]));
    const int  scn = g.vec3(kc(kScene[0]), kc(kScene[1]), kc(kScene[2]));
    const int  dep = g.vec3(kc(kDeep[0]), kc(kDeep[1]), kc(kDeep[2]));
    const int  ext = g.vec3(kc(kExt[0]), kc(kExt[1]), kc(kExt[2]));
    const int  fmc = g.vec3(kc(kFoamC[0]), kc(kFoamC[1]), kc(kFoamC[2]));

    const int node = wt::water_shade(g, nn, vv, ll, sun, sky, scn, dep, ext, dp, wv, fm, fmc, rg);

    double o[kN * 3] = {};
    kir::eval_cpu(g, inp, &alloc, node, o);
    int bad = 0;
    for (int i = 0; i < kN; ++i)
    {
        const double n_i[3] = {nrm[0][i], nrm[1][i], nrm[2][i]};
        const double v_i[3] = {vw[0][i], vw[1][i], vw[2][i]};
        const double l_i[3] = {lw[0][i], lw[1][i], lw[2][i]};
        for (int c = 0; c < 3; ++c)
        {
            if (o[i * 3 + c] != ref_water(n_i, v_i, l_i, dpv[i], wvv[i], fmv[i], rgv[i], c)) { ++bad; }
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("B16-a-4: ocean_sun_glitter (slope-variance GGX sun path) bit-exact vs f64 reference", "[kir][water]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int vx = g.input(sh, kir::DType::F64); const int vy = g.input(sh, kir::DType::F64); const int vz = g.input(sh, kir::DType::F64);
    const int lx = g.input(sh, kir::DType::F64); const int ly = g.input(sh, kir::DType::F64); const int lz = g.input(sh, kir::DType::F64);
    const int vr = g.input(sh, kir::DType::F64);

    double nrm[3][kN]; double vw[3][kN]; double lw[3][kN]; double var[kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        nrm[0][i] = 0.06 * i - 0.5; nrm[1][i] = 1.0;            nrm[2][i] = 0.2 - 0.015 * i;
        vw[0][i]  = 0.3;            vw[1][i]  = 0.8 - 0.02 * i; vw[2][i]  = 0.5 + 0.01 * i;
        lw[0][i]  = 0.4 - 0.04 * i; lw[1][i]  = 0.6;            lw[2][i]  = 0.4;
        unit(nrm[0][i], nrm[1][i], nrm[2][i]); unit(vw[0][i], vw[1][i], vw[2][i]); unit(lw[0][i], lw[1][i], lw[2][i]);
        var[i] = 0.002 + 0.01 * i; // slope variance 0.002..0.19
    }
    const double* inp[] = {nrm[0], nrm[1], nrm[2], vw[0], vw[1], vw[2], lw[0], lw[1], lw[2], var};

    const auto kc  = [&](double x) { return g.constant(x, sh, kir::DType::F64); };
    const int  sun = g.vec3(kc(kSun2[0]), kc(kSun2[1]), kc(kSun2[2]));
    const int  node = wt::ocean_sun_glitter(g, g.vec3(nx, ny, nz), g.vec3(vx, vy, vz), g.vec3(lx, ly, lz), sun, vr);

    double o[kN * 3] = {};
    kir::eval_cpu(g, inp, &alloc, node, o);
    int bad = 0;
    for (int i = 0; i < kN; ++i)
    {
        const double n_i[3] = {nrm[0][i], nrm[1][i], nrm[2][i]};
        const double v_i[3] = {vw[0][i], vw[1][i], vw[2][i]};
        const double l_i[3] = {lw[0][i], lw[1][i], lw[2][i]};
        for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref_glitter(n_i, v_i, l_i, var[i], c)) { ++bad; } }
    }
    CHECK(bad == 0);
}

TEST_CASE("B16-a-4: sky_color (horizon gradient + sun disk) bit-exact vs f64 reference", "[kir][water]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int dx = g.input(sh, kir::DType::F64); const int dy = g.input(sh, kir::DType::F64); const int dz = g.input(sh, kir::DType::F64);
    const int sx = g.input(sh, kir::DType::F64); const int sy = g.input(sh, kir::DType::F64); const int sz = g.input(sh, kir::DType::F64);

    double dir[3][kN]; double sdr[3][kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        dir[0][i] = 0.5 - 0.05 * i; dir[1][i] = 0.1 + 0.045 * i; dir[2][i] = 0.6;
        sdr[0][i] = 0.3;            sdr[1][i] = 0.5;             sdr[2][i] = 0.8 - 0.02 * i;
        unit(dir[0][i], dir[1][i], dir[2][i]); unit(sdr[0][i], sdr[1][i], sdr[2][i]);
    }
    const double* inp[] = {dir[0], dir[1], dir[2], sdr[0], sdr[1], sdr[2]};

    const auto kc  = [&](double x) { return g.constant(x, sh, kir::DType::F64); };
    const int  hor = g.vec3(kc(kHoriz[0]), kc(kHoriz[1]), kc(kHoriz[2]));
    const int  zen = g.vec3(kc(kZen[0]), kc(kZen[1]), kc(kZen[2]));
    const int  sun = g.vec3(kc(kSkySun[0]), kc(kSkySun[1]), kc(kSkySun[2]));
    const int  node = wt::sky_color(g, g.vec3(dx, dy, dz), g.vec3(sx, sy, sz), hor, zen, sun);

    double o[kN * 3] = {};
    kir::eval_cpu(g, inp, &alloc, node, o);
    int bad = 0;
    for (int i = 0; i < kN; ++i)
    {
        const double d_i[3] = {dir[0][i], dir[1][i], dir[2][i]};
        const double s_i[3] = {sdr[0][i], sdr[1][i], sdr[2][i]};
        for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref_sky(d_i, s_i, c)) { ++bad; } }
    }
    CHECK(bad == 0);
}
